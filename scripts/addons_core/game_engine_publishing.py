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

import bpy
import os
import tempfile
import shutil
import tarfile
import time
import stat
import struct
import subprocess
import ctypes
import platform as _platform

bl_info = {
    "name": "Game Engine Publishing",
    "author": "Mitchell Stokes (Moguri), Oren Titane (Genome36), Ghost DEV",
    "version": (0, 2, 0),
    "blender": (5, 0, 0),
    "location": "Render Properties > Publishing Info",
    "description": "Publish .blend file as game engine runtime, manage versions and platforms",
    "warning": "The addon is still under development and you may encounter errors",
    "wiki_url": "http://wiki.blender.org/index.php/Extensions:2.6/Py/Scripts/Game_Engine/Publishing",
    "category": "Game Engine",
}


def WriteRuntime(player_path, output_path, asset_paths, copy_python, overwrite_lib, copy_dlls, make_archive, icon_path="",company_name="", description="", game_version="", game_name="", report=print):


    player_path = bpy.path.abspath(player_path)
    ext = os.path.splitext(player_path)[-1].lower()
    output_path = bpy.path.abspath(output_path)
    upbge_dir = os.path.dirname(bpy.app.binary_path)
    version_string = bpy.app.version_string.split()[0][:3]
    rcedit_path = os.path.join(upbge_dir, version_string, "rceditcustom", "rcedit-x64.exe")
    output_dir = os.path.dirname(output_path)
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)
    
    python_dir = os.path.dirname(_platform.__file__)

    # Check the paths
    if not os.path.isfile(player_path) and not(os.path.exists(player_path) and player_path.endswith('.app')):
        report({'ERROR'}, "The player could not be found! Runtime not saved")
        return

    # Check if we're bundling a .app
    if player_path.lower().endswith('.app'):
        # Python doesn't need to be copied for OS X since it's already inside blenderplayer.app
        copy_python = False

        output_path = bpy.path.ensure_ext(output_path, '.app')

        if os.path.exists(output_path):
            shutil.rmtree(output_path)

        shutil.copytree(player_path, output_path)
        bpy.ops.wm.save_as_mainfile(filepath=os.path.join(output_path, 'Contents', 'Resources', 'game.blend'),
                                    relative_remap=False,
                                    compress=False,
                                    copy=True,
                                    )
    else:
        # Enforce "exe" extension on Windows
        if player_path.lower().endswith('.exe'):
            output_path = bpy.path.ensure_ext(output_path, '.exe')

        # Get the player's binary and the offset for the blend
        with open(player_path, "rb") as file:
            player_d = file.read()
            offset = file.tell()
    # Icon Path 
        if ext == ".exe" and os.path.exists(rcedit_path):
            tmp_player = os.path.join(tempfile.gettempdir(), "tmp_player.exe")
            shutil.copy2(player_path, tmp_player)
            if icon_path: 
                icon_path = bpy.path.abspath(icon_path)
                if os.path.exists(icon_path):
                    if os.path.exists(rcedit_path):
                        rcedit_folder = os.path.join(version_string, "rceditcustom")
                        rcedit_path = os.path.join(upbge_dir, rcedit_folder, "rcedit-x64.exe")
                        subprocess.check_call([rcedit_path, tmp_player, "--set-icon", icon_path])
                        report({'INFO'}, "Icon applied successfully")
                    else:
                        report({'WARNING'}, "rcedit not found, icon not applied")
        
            if company_name:
                subprocess.check_call([rcedit_path, tmp_player, "--set-version-string", "CompanyName", company_name])
            if description:
                subprocess.check_call([rcedit_path, tmp_player, "--set-version-string", "FileDescription", description])
                subprocess.check_call([rcedit_path, tmp_player, "--set-version-string", "ProductName", description])
            if game_version:
                subprocess.check_call([rcedit_path, tmp_player, "--set-file-version", game_version])
                subprocess.check_call([rcedit_path, tmp_player,"--set-product-version", game_version])
            with open(tmp_player, "rb") as f:
                player_d = f.read()
                offset = len(player_d)

        # Create a tmp blend file (Blenderplayer doesn't like compressed blends)
        tempdir = tempfile.mkdtemp()
        blend_path = os.path.join(tempdir, bpy.path.clean_name(output_path))
        bpy.ops.wm.save_as_mainfile(filepath=blend_path,
                                    relative_remap=False,
                                    compress=False,
                                    copy=True,
                                    )

        # Get the blend data
        with open(blend_path, "rb") as blend_file:
            blend_d = blend_file.read()

        # Get rid of the tmp blend, we're done with it
        os.remove(blend_path)
        os.rmdir(tempdir)

        # Create a new file for the bundled runtime
        with open(output_path, "wb") as output:
            # Write the player and blend data to the new runtime
            print("Writing runtime...", end=" ", flush=True)
            output.write(player_d)
            output.write(blend_d)

            # Store the offset (an int is 4 bytes, so we split it up into 4 bytes and save it)
            output.write(struct.pack('BBBB', (offset >> 24) & 0xFF,
                                     (offset >> 16) & 0xFF,
                                     (offset >> 8) & 0xFF,
                                     (offset >> 0) & 0xFF))

            # Stuff for the runtime
            output.write(b'BRUNTIME')

        print("done", flush=True)

    # Make sure the runtime is executable
    os.chmod(output_path, 0o755)
    # Linux write .desktop

    if ext not in ('.exe',) and not player_path.lower().endswith('.app'):
        runtime_name = os.path.splitext(os.path.basename(output_path))[0]
        output_dir = os.path.dirname(output_path)

        if icon_path and os.path.exists(icon_path):
            icon_dst = os.path.join(output_dir, runtime_name + '.png')
            shutil.copy2(icon_path, icon_dst)
            report({'INFO'}, "Linux icon copied")

        desktop_content = "[Desktop Entry]\n"
        desktop_content  += "Type=Application\n"
        if bpy.context.scene.ge_publish_settings.game_name != "":
            desktop_content  += f"Name={bpy.context.scene.ge_publish_settings.game_name}\n"
        else:
            desktop_content  += f"Name={runtime_name}\n"
        if description:
            desktop_content += f"Comment={description}\n"
        if game_version:
            desktop_content += f"Version={game_version}\n"
        desktop_content += f"Exec=./{runtime_name}\n"
        desktop_content += f"Icon={icon_path}\n"
        desktop_content += "Categories=Game;\n"
        desktop_content += "Terminal=false\n"

        desktop_path = os.path.join(output_dir, runtime_name + '.desktop')
        with open(desktop_path, 'w') as f:
            f.write(desktop_content)
        os.chmod(desktop_path, 0o755)
        report({'INFO'}, "Linux .desktop file written")

    # Copy bundled Python
    blender_dir = os.path.dirname(player_path)
    if copy_python:
        print("Copying Python files...", end=" ", flush=True)
        ver = bpy.app.version_string.split()[0][:3]
        # Python libs for Windows
        if ext == ".exe":
            py_folder = os.path.join(ver, "python", "lib")
            src = python_dir
        else:
            # Python libs for linux
            linux_py = os.path.join(blender_dir, ver, "python")
            if os.path.exists(linux_py):
                py_folder = os.path.join(ver, "python")
                src = linux_py
            else:
                py_folder = os.path.join(ver, "python", "lib")
                src = python_dir
        dst = os.path.join(output_dir, py_folder)

        if not os.path.exists(src):
            print("skipped (Python folder not found)", flush=True)
        else:
            if os.path.exists(dst) and overwrite_lib:
                shutil.rmtree(dst)
            if not os.path.exists(dst):
                def _ignore_python(src_dir, names):
                    ingnored = set()
                    for name in names:
                        if name =='__pycache__':
                            ingnored.add(name)
                        elif name == 'site-packages':
                            ingnored.add(name)
                    return ingnored
                def _copy_site_packages(src_sp, dst_sp):
                    keep = { 'numpy', 'numpy-2.3.4dist-info', 'uplogic', 'uplogic-0.15.dist-info'}
                    os.makedirs(dst_sp, exist_ok=True)
                    for item in os.listdir(src_sp):
                        if item in keep:
                            s = os.path.join(src_sp, item)
                            d = os.path.join(dst_sp, item)
                            if os.path.isdir(s):
                                shutil.copytree(s, d, ignore=shutil.ignore_patterns('__pycache__'))
                            else:
                                shutil.copy2(s, d)
                shutil.copytree(src, dst, ignore=_ignore_python)
                src_sp = os.path.join(src, 'site-packages')
                dst_sp = os.path.join(dst, 'site-packages')
                if os.path.exists(src_sp):
                    _copy_site_packages(src_sp, dst_sp)
                print("done", flush=True)
            else:
                print("used existing Python folder", flush=True)

    # And DLLs and files if we're doing a Windows runtime)
    if copy_dlls and ext == ".exe":
        print(f"copy_dlls={copy_dlls}, ext={ext}, blender_dir={blender_dir}")
        print("Copying DLLs...", end=" ", flush=True)
        upbge_dir = os.path.dirname(bpy.app.binary_path)
        
        for file in [i for i in os.listdir(blender_dir) if i.lower().endswith('.dll')]:
            src = os.path.join(blender_dir, file)
            dst = os.path.join(output_dir, file)
            shutil.copy2(src, dst)
        src = os.path.join(upbge_dir, "blender.crt")
        dst = os.path.join(output_dir, "blender.crt")
        if os.path.exists(src) and not os.path.exists(dst):
            shutil.copytree(src, dst)
        src = os.path.join(upbge_dir, "blender.shared")
        dst = os.path.join(output_dir, "blender.shared")
        if os.path.exists(src) and not os.path.exists(dst):
            shutil.copytree(src, dst)
        src = os.path.join(upbge_dir, "license")
        dst = os.path.join(output_dir, "license")
        if os.path.exists(src) and not os.path.exists(dst):
            shutil.copytree(src, dst)
        ver = bpy.app.version_string.split()[0][:3]
        data_subdirs= [
            os.path.join(ver, "datafiles", "colormanagement"),
            os.path.join(ver, "datafiles", "fonts"),
            os.path.join(ver, "datafiles", "gamecontroller"),
        ]
        scripts_subdirs= [
            os.path.join(ver, "scripts", "bge"),
            os.path.join(ver, "scripts", "modules"),
        ]
        python_subdirs= [
            os.path.join(ver, "python", "bin"),
            os.path.join(ver, "python", "DLLs")
        ]
        for subdir in data_subdirs + scripts_subdirs + python_subdirs:
            src = os.path.join(upbge_dir, subdir)
            dst = os.path.join(output_dir, subdir)
            if os.path.exists(src) and not os.path.exists(dst):
                shutil.copytree(src, dst, ignore=shutil.ignore_patterns('site-packages'))
        
        print("done", flush=True)
    
    # Copy libs and files for Linux
    if ext not in ('.exe',) and not player_path.lower().endswith('.app'):
        upbge_dir = os.path.dirname(player_path)

        for file in [i for i in os.listdir(upbge_dir) if i.lower().endswith('.so') or '.so.' in i.lower()]:
            src = os.path.join(upbge_dir, file)
            dst = os.path.join(output_dir, file)
            shutil.copy2(src, dst)

        src = os.path.join(upbge_dir, "lib")
        dst = os.path.join(output_dir, "lib")
        if os.path.exists(src) and not os.path.exists(dst):
            shutil.copytree(src, dst)

        src = os.path.join(upbge_dir, "license")
        dst = os.path.join(output_dir, "license")
        if os.path.exists(src) and not os.path.exists(dst):
            shutil.copytree(src, dst)

        ver = bpy.app.version_string.split()[0][:3]
        linux_subdirs = [
            os.path.join(ver, "datafiles", "colormanagement"),
            os.path.join(ver, "datafiles", "fonts"),
            os.path.join(ver, "datafiles", "gamecontroller"),
            os.path.join(ver, "scripts", "bge"),
            os.path.join(ver, "scripts", "modules"),
            os.path.join(ver, "python", "bin"),
            os.path.join(ver, "python", "lib"),
        ]

        for subdir in linux_subdirs:
            src = os.path.join(upbge_dir, subdir)
            dst = os.path.join(output_dir, subdir)
            if os.path.exists(src) and not os.path.exists(dst):
                shutil.copytree(src, dst, ignore=shutil.ignore_patterns('site-packages'))
                    
        print("done", flush=True)

    # Copy assets
    for ap in asset_paths:
        src = bpy.path.abspath(ap.name)
        dst = os.path.join(output_dir, ap.name[2:] if ap.name.startswith('//') else ap.name)

        if os.path.exists(src):
            if os.path.isdir(src):
                if ap.overwrite and os.path.exists(dst):
                    shutil.rmtree(dst)
                elif not os.path.exists(dst):
                    shutil.copytree(src, dst)
            else:
                if ap.overwrite or not os.path.exists(dst):
                    shutil.copy2(src, dst)
        else:
            report({'ERROR'}, "Could not find asset path: '%s'" % src)

    # Make archive
    if make_archive:
        print("Making archive...", end=" ", flush=True)

        arctype = ''
        if player_path.lower().endswith('.exe'):
            arctype = 'zip'
        elif player_path.lower().endswith('.app'):
            arctype = 'zip'
        else: # Linux
            arctype = 'gztar'

        basedir = os.path.normpath(os.path.join(os.path.dirname(output_path), '..'))
        afilename = os.path.join(basedir, os.path.basename(output_dir))

        if arctype == 'gztar':
            # Create the tarball ourselves instead of using shutil.make_archive
            # so we can handle permission bits.

            # The runtimename needs to use forward slashes as a path separator
            # since this is what tarinfo.name is using.
            runtimename = os.path.relpath(output_path, basedir).replace('\\', '/')

            def _set_ex_perm(tarinfo):
                if tarinfo.name == runtimename:
                    tarinfo.mode = 0o755
                return tarinfo

            with tarfile.open(afilename + '.tar.gz', 'w:gz') as tf:
                tf.add(output_dir, os.path.relpath(output_dir, basedir), filter=_set_ex_perm)
        elif arctype == 'zip':
            shutil.make_archive(afilename, 'zip', output_dir)
        else:
            report({'ERROR'}, "Unknown archive type %s for runtime %s\n" % (arctype, player_path))

        print("done", flush=True)
    # Force Windows icon cache refresh
    if _platform.system() == "Windows":
        os.utime(output_path, None)
        ctypes.windll.shell32.SHChangeNotify(0x00000008, 0x0000, None, None)
        ctypes.windll.user32.UpdateWindow(ctypes.windll.user32.GetShellWindow())


class PublishAllPlatforms(bpy.types.Operator):
    bl_idname = "wm.publish_platforms"
    bl_label = "Exports a runtime for each listed platform"

    def execute(self, context):
        ps = context.scene.ge_publish_settings

        for platform in ps.platforms:
            if platform.publish:
                print("Publishing", platform.name)
                WriteRuntime(platform.player_path,
                            os.path.join(ps.output_path, platform.name, ps.runtime_name),
                            ps.asset_paths,
                            True,
                            True,
                            True,
                            ps.make_archive,
                            platform.icon_path,
                            ps.company_name,
                            ps.description,
                            ps.game_version,
                            self.report
                            )
            else:
                print("Skipping", platform.name)

        return {'FINISHED'}


class RENDER_UL_assets(bpy.types.UIList):
    bl_label = "Asset Paths Listing"

    def draw_item(self, context, layout, data, item, icon, active_data, active_propname):
        layout.prop(item, "name", text="", emboss=False)


class RENDER_UL_platforms(bpy.types.UIList):
    bl_label = "Platforms Listing"

    def draw_item(self, context, layout, data, item, icon, active_data, active_propname):
        row = layout.row()
        row.label(text=item.name)
        row.prop(item, "publish", text="  ")


class RENDER_PT_publish(bpy.types.Panel):
    bl_label = "Game Engine Publishing"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "render"

    @classmethod
    def poll(cls, context):
        scene = context.scene
        return scene is not None

    def draw(self, context):
        ps = context.scene.ge_publish_settings
        layout = self.layout

        # config
        layout.prop(ps, 'output_path')
        layout.prop(ps, 'runtime_name')
        layout.prop(ps, 'lib_path')
        layout.prop(ps, 'make_archive')

        layout.separator()

        # assets list
        layout.label(text="Asset Paths (Under development)")

        # UI_UL_list
        row = layout.row()
        row.template_list("RENDER_UL_assets", "assets_list", ps, 'asset_paths', ps, 'asset_paths_active')

        # operators
        col = row.column(align=True)
        col.operator(PublishAddAssetPath.bl_idname, icon='ADD', text="")
        col.operator(PublishRemoveAssetPath.bl_idname, icon='REMOVE', text="")

        # indexing
        if len(ps.asset_paths) > ps.asset_paths_active >= 0:
            ap = ps.asset_paths[ps.asset_paths_active]
            row = layout.row()
            row.prop(ap, 'overwrite')

        layout.separator()

        # publishing list
        row = layout.row(align=True)
        row.label(text="Platforms")

        # UI_UL_list
        row = layout.row()
        row.template_list("RENDER_UL_platforms", "platforms_list", ps, 'platforms', ps, 'platforms_active')

        # operators
        col = row.column(align=True)
        col.operator(PublishAddPlatform.bl_idname, icon='ADD', text="")
        col.operator(PublishRemovePlatform.bl_idname, icon='REMOVE', text="")
        col.menu("PUBLISH_MT_platform_specials", icon='DOWNARROW_HLT', text="")

        # indexing
        if len(ps.platforms) > ps.platforms_active >= 0:
            platform = ps.platforms[ps.platforms_active]
            layout.prop(platform, 'name')
            layout.prop(platform, 'player_path')
            layout.prop(ps, 'game_name')
            layout.prop(ps, 'company_name')
            layout.prop(ps, 'description')
            layout.prop(ps, 'game_version')
            layout.prop(platform, 'icon_path')

        layout.operator(PublishAllPlatforms.bl_idname, text='Publish Platforms')


class PublishAutoPlatforms(bpy.types.Operator):
    bl_idname = "scene.publish_auto_platforms"
    bl_label = "Auto Add Platforms"

    def execute(self, context):
        ps = context.scene.ge_publish_settings

        # verify lib folder
        lib_path = bpy.path.abspath(ps.lib_path)
        if not os.path.exists(lib_path):
            self.report({'ERROR'}, "Could not add platforms, lib folder (%s) does not exist" % lib_path)
            return {'CANCELLED'}

        for lib in [i for i in os.listdir(lib_path) if os.path.isdir(os.path.join(lib_path, i))]:
            print("Found folder:", lib)
            player_found = False
            for root, dirs, files in os.walk(os.path.join(lib_path, lib)):
                if "__MACOSX" in root:
                    continue

                for f in dirs + files:
                    if f.startswith("blenderplayer.app") or f.startswith("blenderplayer"):
                        a = ps.platforms.add()
                        if lib.startswith('blender-'):
                            # Clean up names for packages from blender.org
                            # example: blender-2.71-RC2-OSX_10.6-x86_64.zip => OSX_10.6-x86_64.zip
                            # We're pretty consistent on naming, so this should hold up.
                            a.name = '-'.join(lib.split('-')[3 if 'rc' in lib.lower() else 2:])
                        else:
                            a.name = lib
                        a.player_path = bpy.path.relpath(os.path.join(root, f))
                        player_found = True
                        break

                if player_found:
                    break

        return {'FINISHED'}

# TODO This operator takes a long time to run, which is bad for UX. Could this instead be done as some sort of
# modal dialog? This could also allow users to select which platforms to download and give a better progress
# indicator.
class PublishDownloadPlatforms(bpy.types.Operator):
    bl_idname = "scene.publish_download_platforms"
    bl_label = "Download Platforms"

    def execute(self, context):
        import html.parser
        import urllib.request

        remote_platforms = []

        ps = context.scene.ge_publish_settings

        # create lib folder if not already available
        lib_path = bpy.path.abspath(ps.lib_path)
        if not os.path.exists(lib_path):
            os.makedirs(lib_path)

        print("Retrieving list of platforms from blender.org...", end=" ", flush=True)

        class AnchorParser(html.parser.HTMLParser):
            def handle_starttag(self, tag, attrs):
                if tag == 'a':
                    for key, value in attrs:
                        if key == 'href' and value.startswith('blender'):
                            remote_platforms.append(value)

        url = 'http://download.blender.org/release/Blender' + bpy.app.version_string.split()[0]
        parser = AnchorParser()
        data = urllib.request.urlopen(url).read()
        parser.feed(str(data))

        print("done", flush=True)

        print("Downloading files (this will take a while depending on your internet connection speed).", flush=True)
        for i in remote_platforms:
            src = '/'.join((url, i))
            dst = os.path.join(lib_path, i)

            dst_dir = '.'.join([i for i in dst.split('.') if i not in {'zip', 'tar', 'bz2'}])
            if not os.path.exists(dst) and not os.path.exists(dst.split('.')[0]):
                print("Downloading " + src + "...", end=" ", flush=True)
                urllib.request.urlretrieve(src, dst)
                print("done", flush=True)
            else:
                print("Reusing existing file: " + dst, flush=True)

            print("Unpacking " + dst + "...", end=" ", flush=True)
            if os.path.exists(dst_dir):
                shutil.rmtree(dst_dir)
            shutil.unpack_archive(dst, dst_dir)
            print("done", flush=True)

        print("Creating platform from libs...", flush=True)
        bpy.ops.scene.publish_auto_platforms()
        return {'FINISHED'}


class PublishAddPlatform(bpy.types.Operator):
    bl_idname = "scene.publish_add_platform"
    bl_label = "Add Publish Platform"

    def execute(self, context):
        a = context.scene.ge_publish_settings.platforms.add()
        a.name = a.name
        return {'FINISHED'}


class PublishRemovePlatform(bpy.types.Operator):
    bl_idname = "scene.publish_remove_platform"
    bl_label = "Remove Publish Platform"

    def execute(self, context):
        ps = context.scene.ge_publish_settings
        if ps.platforms_active < len(ps.platforms):
            ps.platforms.remove(ps.platforms_active)
            return {'FINISHED'}
        return {'CANCELLED'}


# TODO maybe this should display a file browser?
class PublishAddAssetPath(bpy.types.Operator):
    bl_idname = "scene.publish_add_assetpath"
    bl_label = "Add Asset Path"

    def execute(self, context):
        a = context.scene.ge_publish_settings.asset_paths.add()
        a.name = a.name
        return {'FINISHED'}


class PublishRemoveAssetPath(bpy.types.Operator):
    bl_idname = "scene.publish_remove_assetpath"
    bl_label = "Remove Asset Path"

    def execute(self, context):
        ps = context.scene.ge_publish_settings
        if ps.asset_paths_active < len(ps.asset_paths):
            ps.asset_paths.remove(ps.asset_paths_active)
            return {'FINISHED'}
        return {'CANCELLED'}


class PUBLISH_MT_platform_specials(bpy.types.Menu):
    bl_label = "Platform Specials"

    def draw(self, context):
        layout = self.layout
        layout.operator(PublishAutoPlatforms.bl_idname)


class PlatformSettings(bpy.types.PropertyGroup):
    name = bpy.props.StringProperty(
            name = "Platform Name",
            description = "The name of the platform",
            default = "Platform",
            )

    player_path: bpy.props.StringProperty(
            name = "Player Path",
            description = "The path to the Blenderplayer to use for this platform",
            default = "//lib/platform/blenderplayer",
            subtype = 'FILE_PATH',
            )
    
    icon_path: bpy.props.StringProperty(
            name = "Icon path",
            description = "Path to the icon for this paltform",
            default = "",
            subtype = 'FILE_PATH',
    )
    publish: bpy.props.BoolProperty(
            name = "Publish",
            description = "Whether or not to publish to this platform",
            default = True,
            )


class AssetPath(bpy.types.PropertyGroup):
    # TODO This needs a way to be a FILE_PATH or a DIR_PATH
    name = bpy.props.StringProperty(
            name = "Asset Path",
            description = "Path to the asset to be copied",
            default = "//src",
            subtype = 'FILE_PATH',
            )

    overwrite: bpy.props.BoolProperty(
            name = "Overwrite Asset",
            description = "Overwrite the asset if it already exists in the destination folder",
            default = True,
            )


class PublishSettings(bpy.types.PropertyGroup):
    output_path: bpy.props.StringProperty(
            name = "Publish Output",
            description = "Where to publish the game",
            default = "//bin/",
            subtype = 'DIR_PATH',
            )

    runtime_name: bpy.props.StringProperty(
            name = "Runtime name",
            description = "The filename for the created runtime",
            default = "game",
            )

    lib_path: bpy.props.StringProperty(
            name = "Library Path",
            description = "Directory to search for platforms",
            default = "//lib/",
            subtype = 'DIR_PATH',
            )


    platforms: bpy.props.CollectionProperty(type=PlatformSettings, name="Platforms")
    platforms_active: bpy.props.IntProperty()

    asset_paths: bpy.props.CollectionProperty(type=AssetPath, name="Asset Paths")
    asset_paths_active: bpy.props.IntProperty()

    make_archive : bpy.props.BoolProperty(
            name = "Make Archive",
            description = "Create a zip archive of the published game",
            default = False,
            )
    
    game_name : bpy.props.StringProperty(
        name  = "Game name",
        description = "Display name of the game (used in Linux .desktop file)",
        default = ""
    )

    company_name : bpy.props.StringProperty(
        name = "Company name",
        description = "Add Developer or company name",
        default = "",
    )
    
    description : bpy.props.StringProperty(
        name = "Description",
        description = "Game title shown in file properties",
        default = "",
    )

    game_version : bpy.props.StringProperty(
        name = "Version",
        description = "Game version number",
        default = "",
    )

classes = (
    PlatformSettings,
    AssetPath,
    PublishSettings,
    PublishAllPlatforms,
    RENDER_UL_assets,
    RENDER_UL_platforms,
    RENDER_PT_publish,
    PublishAutoPlatforms,
    PublishAddPlatform,
    PublishRemovePlatform,
    PublishAddAssetPath,
    PublishRemoveAssetPath,
    PUBLISH_MT_platform_specials,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)

    bpy.types.Scene.ge_publish_settings = bpy.props.PointerProperty(type=PublishSettings)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
    del bpy.types.Scene.ge_publish_settings


if __name__ == "__main__":
    register()
