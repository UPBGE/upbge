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
"""
An addon for development and test of the generic proxy mechanism
"""
import bpy
import logging
import os
import time

logger = logging.Logger(__name__, logging.INFO)
default_test = "test_module.TestCase.test_name"


class DebugDataProperties(bpy.types.PropertyGroup):
    profile_cumulative: bpy.props.BoolProperty(name="ProfileCumulative", default=False)  # type: ignore
    profile_callers: bpy.props.BoolProperty(name="ProfileCallers", default=False)  # type: ignore
    test_names: bpy.props.StringProperty(name="TestNames", default=default_test)  # type: ignore


proxy = None


class BuildProxyOperator(bpy.types.Operator):
    """Build proxy from current file"""

    bl_idname = "mixer.build_proxy"
    bl_label = "Mixer build proxy"
    bl_options = {"REGISTER"}

    def execute(self, context):
        # Cannot import at module level, since it requires access to bpy.data which is not
        # accessible during module load
        from mixer.blender_data.bpy_data_proxy import BpyDataProxy
        from mixer.blender_data.filter import test_properties
        import cProfile
        import io
        import pstats
        from pstats import SortKey

        global proxy

        profile_cumulative = get_props().profile_cumulative
        profile_callers = get_props().profile_callers
        profile = profile_callers or profile_cumulative
        proxy = BpyDataProxy()
        if profile:
            pr = cProfile.Profile()
            pr.enable()
        t1 = time.time()
        proxy.load(test_properties)
        t2 = time.time()
        if profile:
            pr.disable()
            s = io.StringIO()
            sortby = SortKey.CUMULATIVE
            ps = pstats.Stats(pr, stream=s).sort_stats(sortby)
            if profile_cumulative:
                ps.print_stats()
            if profile_callers:
                ps.print_callers()
            print(s.getvalue())

        logger.warning(f"Elapse: {t2 - t1} s.")

        # Put breakpoint here and examinate non_empty dictionnary
        return {"FINISHED"}


class DiffProxyOperator(bpy.types.Operator):
    """Diff proxy """

    bl_idname = "mixer.diff_proxy"
    bl_label = "Mixer diff proxy"
    bl_options = {"REGISTER"}

    def execute(self, context):
        # Cannot import at module level, since it requires access to bpy.data which is not
        # accessible during module load
        import cProfile
        import io
        import pstats
        from pstats import SortKey

        global proxy

        profile_cumulative = get_props().profile_cumulative
        profile_callers = get_props().profile_callers
        profile = profile_callers or profile_cumulative
        if profile:
            pr = cProfile.Profile()
            pr.enable()
        t1 = time.time()
        _ = proxy.data("scenes").search_one("Scene").diff(bpy.data.scenes["Scene"], "Scene", None, proxy.context())
        _ = proxy.data("objects").search_one("Cube").diff(bpy.data.objects["Cube"], "Cube", None, proxy.context())
        t2 = time.time()
        if profile:
            pr.disable()
            s = io.StringIO()
            sortby = SortKey.CUMULATIVE
            ps = pstats.Stats(pr, stream=s).sort_stats(sortby)
            if profile_cumulative:
                ps.print_stats()
            if profile_callers:
                ps.print_callers()
            print(s.getvalue())

        logger.warning(f"Elapse: {t2 - t1} s.")

        # Put breakpoint here and examinate non_empty dictionnary
        return {"FINISHED"}


class DebugDataTestOperator(bpy.types.Operator):
    """Execute blender_data tests for debugging"""

    bl_idname = "mixer.data_test"
    bl_label = "Mixer test data"
    bl_options = {"REGISTER"}

    def execute(self, context):
        # Cannot import at module level, since it requires access to bpy.data which is not
        # accessible during module load
        from mixer.blender_data.tests.utils import run_tests

        names = get_props().test_names
        if names:
            base = "mixer.blender_data.tests."
            test_names = [base + name for name in names.split()]
        else:
            test_names = None

        run_tests(test_names)

        return {"FINISHED"}


class DebugDataPanel(bpy.types.Panel):
    """blender_data debug Panel"""

    bl_label = "Data"
    bl_idname = "DATA_PT_settings"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Mixer"

    def draw(self, context):
        layout = self.layout

        row = layout.column()
        row.operator(BuildProxyOperator.bl_idname, text="Build Proxy")
        row.operator(DiffProxyOperator.bl_idname, text="Diff Proxy")
        row.prop(get_props(), "profile_cumulative", text="Profile Cumulative")
        row.prop(get_props(), "profile_callers", text="Profile callers")
        row.operator(DebugDataTestOperator.bl_idname, text="Test")
        row = layout.row()
        row.prop(get_props(), "test_names", text="Test")


classes = (
    BuildProxyOperator,
    DiffProxyOperator,
    DebugDataTestOperator,
    DebugDataPanel,
    DebugDataProperties,
)

use_debug_addon = os.environ.get("MIXER_DEVELOPMENT", False)


def get_props() -> DebugDataProperties:
    return bpy.context.window_manager.debug_data_props


def register():
    if use_debug_addon:
        for class_ in classes:
            bpy.utils.register_class(class_)
        bpy.types.WindowManager.debug_data_props = bpy.props.PointerProperty(type=DebugDataProperties)


def unregister():
    if use_debug_addon:
        for class_ in classes:
            bpy.utils.unregister_class(class_)
