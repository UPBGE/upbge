import bpy
import bge

def WindowSize():
    own = bge.logic.getCurrentController().owner
    bge.render.setWindowSize (int(own['WindowSizeX']),int(own['WindowSizeY']))

def VSYNC():
    own = bge.logic.getCurrentController().owner
    if own['VSYNC'] == 0:
        bge.render.setVsync (VSYNC_OFF)
    elif own['VSYNC'] == 1:
        bge.render.setVsync (VSYNC_ON)
    elif own['VSYNC'] == 2:
        bge.render.setVsync (VSYNC_ADAPTIVE)

def FullScreen():
    own = bge.logic.getCurrentController().owner
    if own['FullScreen'] == True:
        bge.render.setFullScreen (True)
    else:
        bge.render.setFullScreen (False)

def FrameRate():
    own = bge.logic.getCurrentController().owner
    if own['FrameRate'] == True:
        bge.render.showFramerate (True)
    else:
        bge.render.showFramerate (False)

def FilmicLook():
    own = bge.logic.getCurrentController().owner
    if own['FilmicLook'] == 0:
        bpy.context.scene.view_settings.look = 'Very Low Contrast'
    elif own['FilmicLook'] == 1:
        bpy.context.scene.view_settings.look = 'Low Contrast'
    elif own['FilmicLook'] == 2:
        bpy.context.scene.view_settings.look = 'Medium Low Contrast'
    elif own['FilmicLook'] == 3:
        bpy.context.scene.view_settings.look = 'Medium Contrast'
    elif own['FilmicLook'] == 4:
        bpy.context.scene.view_settings.look = 'Medium High Contrast'
    elif own['FilmicLook'] == 5:
        bpy.context.scene.view_settings.look = 'High Contrast'
    elif own['FilmicLook'] == 6:
        bpy.context.scene.view_settings.look = 'Very High Contrast'

def Exposure():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.view_settings.exposure = float(own['Exposure'])

def Gamma():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.view_settings.gamma = float(own['Gamma'])

def OverScan():
    own = bge.logic.getCurrentController().owner
    if own['OverScan'] == True:
        bpy.context.scene.eevee.use_overscan = True
    else:
        bpy.context.scene.eevee.use_overscan = False

def OverScanSize():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.eevee.overscan_size = int(own['OverScanSize'])

def MouseCursor():
    own = bge.logic.getCurrentController().owner
    if own['MouseCursor'] == True:
        bge.render.showMouse (True)
    else:
        bge.render.showMouse (False)

def MakeScreenShot():
    own = bge.logic.getCurrentController().owner
    bge.render.makeScreenshot ('//ScreenShots/ScreenShot-#')

def AnisotropicFiltering():
    own = bge.logic.getCurrentController().owner
    if own['AnisotropicFiltering'] == 0:
        bge.render.setAnisotropicFiltering (1)
    elif own['AnisotropicFiltering'] == 1:
        bge.render.setAnisotropicFiltering (2)
    elif own['AnisotropicFiltering'] == 2:
        bge.render.setAnisotropicFiltering (4)
    elif own['AnisotropicFiltering'] == 3:
        bge.render.setAnisotropicFiltering (8)
    elif own['AnisotropicFiltering'] == 4:
        bge.render.setAnisotropicFiltering (16)

def RenderSamples():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.eevee.taa_render_samples = int(own['RenderSamples'])
    bpy.context.scene.eevee.taa_samples = int(own['RenderSamples'])

def SMAA():
    own = bge.logic.getCurrentController().owner
    if own['SMAA'] == True:
        bpy.context.scene.eevee.use_eevee_smaa = True
    else:
        bpy.context.scene.eevee.use_eevee_smaa = False

def GTAO():
    own = bge.logic.getCurrentController().owner
    if own['GTAO'] == True:
        bpy.context.scene.eevee.use_gtao = True
    else:
        bpy.context.scene.eevee.use_gtao = False

def GTAOBentNormals():
    own = bge.logic.getCurrentController().owner
    if own['GTAOBentNormals'] == True:
        bpy.context.scene.eevee.use_gtao_bent_normals = True
    else:
        bpy.context.scene.eevee.use_gtao_bent_normals = False

def GTAOBounce():
    own = bge.logic.getCurrentController().owner
    if own['GTAOBounce'] == True:
        bpy.context.scene.eevee.use_gtao_bounce = True
    else:
        bpy.context.scene.eevee.use_gtao_bounce = False

def GTAOQuality():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.eevee.gtao_quality = float(own['GTAOQuality'])

def Bloom():
    own = bge.logic.getCurrentController().owner
    if own['Bloom'] == True:
        bpy.context.scene.eevee.use_bloom = True
    else:
        bpy.context.scene.eevee.use_bloom = False

def BloomRadius():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.eevee.bloom_radius = float(own['BloomRadius'])

def BloomIntensity():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.eevee.bloom_intensity = float(own['BloomIntensity'])

def SSSSamples():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.eevee.sss_samples = int(own['SSSSamples'])

def SSR():
    own = bge.logic.getCurrentController().owner
    if own['SSR'] == True:
        bpy.context.scene.eevee.use_ssr = True
    else:
        bpy.context.scene.eevee.use_ssr = False

def SSRRefraction():
    own = bge.logic.getCurrentController().owner
    if own['SSRRefraction'] == True:
        bpy.context.scene.eevee.use_ssr_refraction = True
    else:
        bpy.context.scene.eevee.use_ssr_refraction = False

def SSRHalfResolution():
    own = bge.logic.getCurrentController().owner
    if own['SSRHalfResolution'] == True:
        bpy.context.scene.eevee.use_ssr_halfres = True
    else:
        bpy.context.scene.eevee.use_ssr_halfres = False

def SSRQuality():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.eevee.ssr_quality = float(own['SSRQuality'])

def MotionBlur():
    own = bge.logic.getCurrentController().owner
    if own['MotionBlur'] == True:
        bpy.context.scene.eevee.use_motion_blur = True
    else:
        bpy.context.scene.eevee.use_motion_blur = False

def MotionBlurSamples():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.eevee.motion_blur_samples = int(own['MotionBlurSamples'])

def MotionBlurShutter():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.eevee.motion_blur_shutter = float(own['MotionBlurShutter'])

def VolumetricTileSize():
    own = bge.logic.getCurrentController().owner
    if own['VolumetricTileSize'] == 0:
        bpy.context.scene.eevee.volumetric_tile_size = '2'
    elif own['VolumetricTileSize'] == 1:
        bpy.context.scene.eevee.volumetric_tile_size = '4'
    elif own['VolumetricTileSize'] == 2:
        bpy.context.scene.eevee.volumetric_tile_size = '8'
    elif own['VolumetricTileSize'] == 3:
        bpy.context.scene.eevee.volumetric_tile_size = '16'

def VolumetricSamples():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.eevee.volumetric_samples = int(own['VolumetricSamples'])

def VolumetricShadows():
    own = bge.logic.getCurrentController().owner
    if own['VolumetricShadows'] == True:
        bpy.context.scene.eevee.use_volumetric_shadows = True
    else:
        bpy.context.scene.eevee.use_volumetric_shadows = False

def VolumetricShadowSamples():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.eevee.volumetric_shadow_samples = int(own['VolumetricShadowSamples'])

def HighQualityNormals():
    own = bge.logic.getCurrentController().owner
    if own['HighQualityNormals'] == True:
        bpy.context.scene.render.use_high_quality_normals = True
    else:
        bpy.context.scene.render.use_high_quality_normals = False

def ShadowCubeSize():
    own = bge.logic.getCurrentController().owner
    if own['ShadowCubeSize'] == 0:
        bpy.context.scene.eevee.shadow_cube_size = '64'
    elif own['ShadowCubeSize'] == 1:
        bpy.context.scene.eevee.shadow_cube_size = '128'
    elif own['ShadowCubeSize'] == 2:
        bpy.context.scene.eevee.shadow_cube_size = '256'
    elif own['ShadowCubeSize'] == 3:
        bpy.context.scene.eevee.shadow_cube_size = '512'
    elif own['ShadowCubeSize'] == 4:
        bpy.context.scene.eevee.shadow_cube_size = '1024'
    elif own['ShadowCubeSize'] == 5:
        bpy.context.scene.eevee.shadow_cube_size = '2048'
    elif own['ShadowCubeSize'] == 6:
        bpy.context.scene.eevee.shadow_cube_size = '4096'

def ShadowCascadeSize():
    own = bge.logic.getCurrentController().owner
    if own['ShadowCascadeSize'] == 0:
        bpy.context.scene.eevee.shadow_cascade_size = '64'
    elif own['ShadowCascadeSize'] == 1:
        bpy.context.scene.eevee.shadow_cascade_size = '128'
    elif own['ShadowCascadeSize'] == 2:
        bpy.context.scene.eevee.shadow_cascade_size = '256'
    elif own['ShadowCascadeSize'] == 3:
        bpy.context.scene.eevee.shadow_cascade_size = '512'
    elif own['ShadowCascadeSize'] == 4:
        bpy.context.scene.eevee.shadow_cascade_size = '1024'
    elif own['ShadowCascadeSize'] == 5:
        bpy.context.scene.eevee.shadow_cascade_size = '2048'
    elif own['ShadowCascadeSize'] == 6:
        bpy.context.scene.eevee.shadow_cascade_size = '4096'

def ShadowHighBitDepth():
    own = bge.logic.getCurrentController().owner
    if own['ShadowHighBitDepth'] == True:
        bpy.context.scene.eevee.use_shadow_high_bitdepth = True
    else:
        bpy.context.scene.eevee.use_shadow_high_bitdepth = False

def SoftShadows():
    own = bge.logic.getCurrentController().owner
    if own['SoftShadows'] == True:
        bpy.context.scene.eevee.use_soft_shadows = True
    else:
        bpy.context.scene.eevee.use_soft_shadows = False
