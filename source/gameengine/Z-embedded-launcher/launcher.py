import bpy, os


def setFullScreen():
    for window in bpy.context.window_manager.windows:
        screen = window.screen

        for area in screen.areas:
            if area.type == 'VIEW_3D':
                override = {'window': window, 'screen': screen, 'area': area}
                bpy.ops.screen.screen_full_area(override)
                break

def cameraView():
    for area in bpy.context.screen.areas:
        if area.type == "VIEW_3D":
            break

    for region in area.regions:
        if region.type == "WINDOW":
            break

    space = area.spaces[0]

    context = bpy.context.copy()
    context['area'] = area
    context['region'] = region
    context['space_data'] = space

    bpy.ops.view3d.view_camera(context, 'EXEC_DEFAULT')

def quitBlenderAtExit(dummy):

    bpy.ops.wm.quit_blender()

def startEmbedded():
    bpy.ops.view3d.game_start()

path = os.getcwd() + "\\test.blend"
bpy.ops.wm.open_mainfile(filepath=path)
setFullScreen()
cameraView()
bpy.app.handlers.game_post.append(quitBlenderAtExit)
startEmbedded()