# ##### BEGIN GPL LICENSE BLOCK #####
#
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software Foundation,
#  Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# ##### END GPL LICENSE BLOCK #####

bl_info = {
    "name": "Save As Game Engine Runtime",
    "author": "Mitchell Stokes (Moguri), Ulysse Martin (youle), Jorge Bernal (lordloki)",
    "version": (0, 9, 0),
    "blender": (2, 80, 0),
    "location": "File > Import-Export",
    "description": "Bundle a .blend file with the Blenderplayer",
    "warning": "",
    "wiki_url": "http://wiki.blender.org/index.php/Extensions:2.6/Py/"
                "Scripts/Game_Engine/Save_As_Runtime",
    "category": "Import-Export",
}

import bpy
import os
import sys
import shutil
import tempfile
import subprocess


def CopyPythonLibs(dst, overwrite_lib, report=print):
    import platform

    # use python module to find python's libpath
    src = os.path.dirname(platform.__file__)

    # dst points to lib/, but src points to current python's library path, eg:
    #  '/usr/lib/python3.2' vs '/usr/lib'
    # append python's library dir name to destination, so only python's
    # libraries would be copied
    if os.name == 'posix':
        dst = os.path.join(dst, os.path.basename(src))

    if os.path.exists(src):
        write = False
        if os.path.exists(dst):
            if overwrite_lib:
                shutil.rmtree(dst)
                write = True
        else:
            write = True
        if write:
            shutil.copytree(src, dst, ignore=lambda dir, contents: [i for i in contents if i == '__pycache__'])
    else:
        report({'WARNING'}, "Python not found in %r, skipping python copy" % src)


def WriteAppleRuntime(player_path, output_path, copy_python, overwrite_lib):
    # Enforce the extension
    if not output_path.endswith('.app'):
        output_path += '.app'

    # Use the system's cp command to preserve some meta-data
    os.system('cp -R "%s" "%s"' % (player_path, output_path))

    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(output_path, "Contents/Resources/game.blend"),
                                relative_remap=False,
                                compress=False,
                                copy=True,
                                )

    # Python doesn't need to be copied for OS X since it's already inside blenderplayer.app


def WriteRuntime(player_path, output_path, new_icon_path, copy_python, overwrite_lib, copy_dlls, copy_libs, copy_scripts, copy_datafiles, copy_modules, copy_logic_nodes, report=print):
    import struct

    # Check the paths
    if not os.path.isfile(player_path) and not(os.path.exists(player_path) and player_path.endswith('.app')):
        report({'ERROR'}, "The player could not be found! Runtime not saved")
        return

    # Check if we're bundling a .app
    if player_path.endswith('.app'):
        WriteAppleRuntime(player_path, output_path, copy_python, overwrite_lib)
        return

    # Enforce "exe" extension on Windows
    if player_path.endswith('.exe') and not output_path.endswith('.exe'):
        output_path += '.exe'

    # Setup main folders
    blender_dir = os.path.dirname(bpy.app.binary_path)
    runtime_dir = os.path.dirname(output_path)

    # Extract new version string. Only take first 3 digits (i.e 3.0)
    string = bpy.app.version_string.split()[0]
    version_string = string[:3]

    # Create temporal directory
    tempdir = tempfile.mkdtemp()
    player_path_temp = player_path

    # Change the icon for Windows
    if (new_icon_path != '' and output_path.endswith('.exe')):
        player_path_temp = os.path.join(tempdir, bpy.path.clean_name(player_path))
        shutil.copyfile(player_path, player_path_temp)
        rcedit_folder = os.path.join(version_string, "rceditcustom")
        rcedit_path = os.path.join(blender_dir, rcedit_folder, "rcedit-x64.exe")
        subprocess.check_call([rcedit_path, player_path_temp, "--set-icon", new_icon_path])

    # Get the player's binary and the offset for the blend
    file = open(player_path_temp, 'rb')
    player_d = file.read()
    offset = file.tell()
    file.close()

    # Create a tmp blend file (Blenderplayer doesn't like compressed blends)
    blend_path = os.path.join(tempdir, bpy.path.clean_name(output_path))
    bpy.ops.wm.save_as_mainfile(filepath=blend_path,
                                relative_remap=False,
                                compress=False,
                                copy=True,
                                )

    # Get the blend data
    blend_file = open(blend_path, 'rb')
    blend_d = blend_file.read()
    blend_file.close()

    # Get rid of the tmp blend, we're done with it
    os.remove(blend_path)
    if (new_icon_path != '' and output_path.endswith('.exe')):
        os.remove(player_path_temp)
    os.rmdir(tempdir)

    # Create a new file for the bundled runtime
    output = open(output_path, 'wb')

    # Write the player and blend data to the new runtime
    print("Writing runtime...", end=" ")
    output.write(player_d)
    output.write(blend_d)

    # Store the offset (an int is 4 bytes, so we split it up into 4 bytes and save it)
    output.write(struct.pack('B', (offset>>24)&0xFF))
    output.write(struct.pack('B', (offset>>16)&0xFF))
    output.write(struct.pack('B', (offset>>8)&0xFF))
    output.write(struct.pack('B', (offset>>0)&0xFF))

    # Stuff for the runtime
    output.write(b'BRUNTIME')
    output.close()

    print("done")

    # Make the runtime executable on Linux
    if os.name == 'posix':
        os.chmod(output_path, 0o755)

    if copy_python:
        print("Copying Python files...", end=" ")
        py_folder = os.path.join(version_string, "python", "lib")
        dst = os.path.join(runtime_dir, py_folder)
        CopyPythonLibs(dst, overwrite_lib, report)
        if output_path.endswith('.exe'):
            py_folder = os.path.join(version_string, "python", "DLLs")
            src = os.path.join(blender_dir, py_folder)
            dst = os.path.join(runtime_dir, py_folder)
            shutil.copytree(src, dst)
        print("done")

    # Copy DLLs
    if copy_dlls:
        print("Copying DLLs...", end=" ")
        # Dlls at executable level
        for file in [i for i in os.listdir(blender_dir) if i.lower().endswith('.dll')]:
            src = os.path.join(blender_dir, file)
            dst = os.path.join(runtime_dir, file)
            shutil.copy2(src, dst)
        # blender.crt DLLs
        src = os.path.join(blender_dir, "blender.crt")
        dst = os.path.join(runtime_dir, "blender.crt")
        shutil.copytree(src, dst)
        # blender.shared DLLs
        src = os.path.join(blender_dir, "blender.shared")
        dst = os.path.join(runtime_dir, "blender.shared")
        shutil.copytree(src, dst)
        print("done")

    # Copy linux shared libs
    if copy_libs:
        print("Copying shared libs...", end=" ")
        # blender.crt DLLs
        src = os.path.join(blender_dir, "lib")
        dst = os.path.join(runtime_dir, "lib")
        shutil.copytree(src, dst)
        print("done")

    # Copy Scripts folder (also copy this folder when logic nodes option is selected)
    if copy_scripts or copy_logic_nodes:
        print("Copying scripts and modules...", end=" ")
        scripts_folder = os.path.join(version_string, "scripts")
        src = os.path.join(blender_dir, scripts_folder)
        dst = os.path.join(runtime_dir, scripts_folder)
        shutil.copytree(src, dst)
        print("done")
        print("Copying userpref.blend to can use addons...", end=" ")
        user_path = bpy.utils.resource_path('USER')
        user_config_path = os.path.join(user_path, "config")
        user_config_userpref_path = os.path.join(user_config_path, "userpref.blend")
        runtime_config_folder = os.path.join(version_string, "config")
        runtime_config_folder_path = os.path.join(runtime_dir, runtime_config_folder)
        os.makedirs(runtime_config_folder_path)
        shutil.copy2(user_config_userpref_path, runtime_config_folder_path)
        print("done")

    # Copy logic nodes game folder
    if copy_logic_nodes:
        print("Copying logic nodes game folder...", end=" ")
        blend_directory = os.path.dirname(bpy.data.filepath)
        src = os.path.join(blend_directory, "bgelogic")
        if os.path.exists(src):
            dst = os.path.join(runtime_dir, "bgelogic")
            shutil.copytree(src, dst)
        print("done")

    # Copy datafiles folder
    if copy_datafiles:
        print("Copying datafiles...", end=" ")
        datafiles_folder = os.path.join(version_string, "datafiles", "gamecontroller")
        src = os.path.join(blender_dir, datafiles_folder)
        dst = os.path.join(runtime_dir, datafiles_folder)
        shutil.copytree(src, dst)
        datafiles_folder = os.path.join(version_string, "datafiles", "colormanagement")
        src = os.path.join(blender_dir, datafiles_folder)
        dst = os.path.join(runtime_dir, datafiles_folder)
        shutil.copytree(src, dst)
        datafiles_folder = os.path.join(version_string, "datafiles", "fonts")
        src = os.path.join(blender_dir, datafiles_folder)
        dst = os.path.join(runtime_dir, datafiles_folder)
        shutil.copytree(src, dst)
        datafiles_folder = os.path.join(version_string, "datafiles", "studiolights")
        src = os.path.join(blender_dir, datafiles_folder)
        dst = os.path.join(runtime_dir, datafiles_folder)
        shutil.copytree(src, dst)
        print("done")

    # Copy modules folder (to have bpy working)
    if copy_modules and not (copy_scripts or copy_logic_nodes):
        print("Copying modules...", end=" ")
        modules_folder = os.path.join(version_string, "scripts", "modules")
        src = os.path.join(blender_dir, modules_folder)
        dst = os.path.join(runtime_dir, modules_folder)
        shutil.copytree(src, dst)
        print("done")

    # Copy license folder
    print("Copying UPBGE license folder...", end=" ")
    src = os.path.join(blender_dir, "license")
    dst = os.path.join(runtime_dir, "engine.license")
    shutil.copytree(src, dst)
    license_folder = os.path.join(runtime_dir, "engine.license")
    src = os.path.join(blender_dir, "copyright.txt")
    dst = os.path.join(license_folder, "copyright.txt")
    shutil.copy2(src, dst)
    print("done")

from bpy.props import *


class SaveAsRuntime(bpy.types.Operator):
    bl_idname = "wm.save_as_runtime"
    bl_label = "Save As Game Engine Runtime"
    bl_options = {'REGISTER'}

    if sys.platform == 'darwin':
        blender_bin_dir = '/' + os.path.join(*bpy.app.binary_path.split('/')[0:-4])
        ext = '.app'
        blenderplayer_name = 'Blenderplayer'
    else:
        blender_bin_path = bpy.app.binary_path
        blender_bin_dir = os.path.dirname(blender_bin_path)
        ext = os.path.splitext(blender_bin_path)[-1].lower()
        blenderplayer_name = 'blenderplayer'

    default_player_path = os.path.join(blender_bin_dir, blenderplayer_name + ext)
    player_path: StringProperty(
            name="Player Path",
            description="The path to the player to use",
            default=default_player_path,
            subtype='FILE_PATH',
            )
    filepath: StringProperty(
            subtype='FILE_PATH',
            )
    copy_python: BoolProperty(
            name="Copy Python",
            description="Copy bundle Python with the runtime",
            default=True,
            )
    overwrite_lib: BoolProperty(
            name="Overwrite 'lib' folder",
            description="Overwrites the lib folder (if one exists) with the bundled Python lib folder",
            default=False,
            )
    copy_scripts: BoolProperty(
            name="Copy Scripts folder",
            description="Copy bundle Scripts folder with the runtime",
            default=False,
            )
    copy_datafiles: BoolProperty(
            name="Copy Datafiles folder",
            description="Copy bundle datafiles folder with the runtime",
            default=True,
            )
    copy_modules: BoolProperty(
            name="Copy Script>Modules folder",
            description="Copy bundle modules folder with the runtime",
            default=True,
            )
    copy_logic_nodes: BoolProperty(
            name="Copy Logic Nodes game folder",
            description="Copy Logic Nodes game with the runtime",
            default=True,
            )

    # Only Windows has dlls to copy or can modify icon
    if ext == '.exe':
        copy_dlls: BoolProperty(
                name="Copy DLLs",
                description="Copy all needed DLLs with the runtime",
                default=True,
                )
        new_icon_path: StringProperty(
                name="New Icon Path",
                description="The path to the new icon for player to use",
                default="",
                subtype='FILE_PATH',
                )
    else:
        copy_dlls = False
        new_icon_path = False

    # Only Linux has lib folder
    if os.name == 'posix':
        copy_libs: BoolProperty(
                name="Copy shared libs",
                description="Copy all the needed executable shared libs with the runtime",
                default=True,
                )
    else:
        copy_libs = False

    def execute(self, context):
        import time
        start_time = time.time()
        print("Saving runtime to %r" % self.filepath)
        WriteRuntime(self.player_path,
                     self.filepath,
                     self.new_icon_path,
                     self.copy_python,
                     self.overwrite_lib,
                     self.copy_dlls,
                     self.copy_libs,
                     self.copy_scripts,
                     self.copy_datafiles,
                     self.copy_modules,
                     self.copy_logic_nodes,
                     self.report,
                     )
        print("Finished in %.4fs" % (time.time() - start_time))
        return {'FINISHED'}

    def invoke(self, context, event):
        if not self.filepath:
            ext = '.app' if sys.platform == 'darwin' else os.path.splitext(bpy.app.binary_path)[-1]
            self.filepath = bpy.path.ensure_ext(bpy.data.filepath, ext)

        wm = context.window_manager
        wm.fileselect_add(self)
        return {'RUNNING_MODAL'}


def menu_func_export(self, context):
    self.layout.operator(SaveAsRuntime.bl_idname, text="Save as game runtime")

classes = (
SaveAsRuntime,
)

def register():
    for cls in classes:
        bpy.utils.register_class(cls)

    bpy.types.TOPBAR_MT_file_export.append(menu_func_export)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(menu_func_export)

    for cls in classes:
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
