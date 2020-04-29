import bpy
import bge

################################ UPBGE API START ################################

def WindowSize():
    own = bge.logic.getCurrentController().owner
    bge.render.setWindowSize (int(own['WindowSizeX']),int(own['WindowSizeY']))

def VSYNC():
    own = bge.logic.getCurrentController().owner
    bge.render.setVsync (int(own['VSYNC']))

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
    AF = [1, 2, 4, 8, 16]
    bge.render.setAnisotropicFiltering (AF[(own['AnisotropicFiltering'])])

################################ UPBGE API END ################################

################################ BLENDER API START ################################

def FilmicLook():
    own = bge.logic.getCurrentController().owner
    FL = ['Very Low Contrast', 'Low Contrast', 'Medium Low Contrast', 'Medium Contrast', 'Medium High Contrast', 'High Contrast', 'Very High Contrast']
    bpy.context.scene.view_settings.look = (FL[(own['FilmicLook'])])
    bge.logic.getCurrentScene().resetTaaSamples = True

def Exposure():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.view_settings.exposure = float(own['Exposure'])
    bge.logic.getCurrentScene().resetTaaSamples = True

def Gamma():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.view_settings.gamma = float(own['Gamma'])
    bge.logic.getCurrentScene().resetTaaSamples = True

def OverScan():
    own = bge.logic.getCurrentController().owner
    if own['OverScan'] == True:
        bpy.context.scene.eevee.use_overscan = True
    else:
        bpy.context.scene.eevee.use_overscan = False
        bge.logic.getCurrentScene().resetTaaSamples = True

def OverScanSize():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.eevee.overscan_size = int(own['OverScanSize'])
    bge.logic.getCurrentScene().resetTaaSamples = True

def RenderSamples():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.eevee.taa_render_samples = int(own['RenderSamples'])
    bge.logic.getCurrentScene().resetTaaSamples = True

def SMAA():
    own = bge.logic.getCurrentController().owner
    if own['SMAA'] == True:
        bpy.context.scene.eevee.use_eevee_smaa = True
    else:
        bpy.context.scene.eevee.use_eevee_smaa = False
        bge.logic.getCurrentScene().resetTaaSamples = True

def GTAO():
    own = bge.logic.getCurrentController().owner
    if own['GTAO'] == True:
        bpy.context.scene.eevee.use_gtao = True
    else:
        bpy.context.scene.eevee.use_gtao = False
        bge.logic.getCurrentScene().resetTaaSamples = True

def GTAOBentNormals():
    own = bge.logic.getCurrentController().owner
    if own['GTAOBentNormals'] == True:
        bpy.context.scene.eevee.use_gtao_bent_normals = True
    else:
        bpy.context.scene.eevee.use_gtao_bent_normals = False
        bge.logic.getCurrentScene().resetTaaSamples = True

def GTAOBounce():
    own = bge.logic.getCurrentController().owner
    if own['GTAOBounce'] == True:
        bpy.context.scene.eevee.use_gtao_bounce = True
    else:
        bpy.context.scene.eevee.use_gtao_bounce = False
        bge.logic.getCurrentScene().resetTaaSamples = True

def GTAOQuality():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.eevee.gtao_quality = float(own['GTAOQuality'])
    bge.logic.getCurrentScene().resetTaaSamples = True

def Bloom():
    own = bge.logic.getCurrentController().owner
    if own['Bloom'] == True:
        bpy.context.scene.eevee.use_bloom = True
    else:
        bpy.context.scene.eevee.use_bloom = False
        bge.logic.getCurrentScene().resetTaaSamples = True

def BloomRadius():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.eevee.bloom_radius = float(own['BloomRadius'])
    bge.logic.getCurrentScene().resetTaaSamples = True

def BloomIntensity():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.eevee.bloom_intensity = float(own['BloomIntensity'])
    bge.logic.getCurrentScene().resetTaaSamples = True

def SSSSamples():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.eevee.sss_samples = int(own['SSSSamples'])
    bge.logic.getCurrentScene().resetTaaSamples = True

def SSR():
    own = bge.logic.getCurrentController().owner
    if own['SSR'] == True:
        bpy.context.scene.eevee.use_ssr = True
    else:
        bpy.context.scene.eevee.use_ssr = False
        bge.logic.getCurrentScene().resetTaaSamples = True

def SSRRefraction():
    own = bge.logic.getCurrentController().owner
    if own['SSRRefraction'] == True:
        bpy.context.scene.eevee.use_ssr_refraction = True
    else:
        bpy.context.scene.eevee.use_ssr_refraction = False
        bge.logic.getCurrentScene().resetTaaSamples = True

def SSRHalfResolution():
    own = bge.logic.getCurrentController().owner
    if own['SSRHalfResolution'] == True:
        bpy.context.scene.eevee.use_ssr_halfres = True
    else:
        bpy.context.scene.eevee.use_ssr_halfres = False
        bge.logic.getCurrentScene().resetTaaSamples = True

def SSRQuality():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.eevee.ssr_quality = float(own['SSRQuality'])
    bge.logic.getCurrentScene().resetTaaSamples = True

def MotionBlur():
    own = bge.logic.getCurrentController().owner
    if own['MotionBlur'] == True:
        bpy.context.scene.eevee.use_motion_blur = True
    else:
        bpy.context.scene.eevee.use_motion_blur = False
        bge.logic.getCurrentScene().resetTaaSamples = True

def MotionBlurSamples():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.eevee.motion_blur_samples = int(own['MotionBlurSamples'])
    bge.logic.getCurrentScene().resetTaaSamples = True

def MotionBlurShutter():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.eevee.motion_blur_shutter = float(own['MotionBlurShutter'])
    bge.logic.getCurrentScene().resetTaaSamples = True

def VolumetricTileSize():
    own = bge.logic.getCurrentController().owner
    VTS = ['2', '4', '8', '16']
    bpy.context.scene.eevee.volumetric_tile_size = (VTS[(own['VolumetricTileSize'])])
    bge.logic.getCurrentScene().resetTaaSamples = True

def VolumetricSamples():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.eevee.volumetric_samples = int(own['VolumetricSamples'])
    bge.logic.getCurrentScene().resetTaaSamples = True

def VolumetricShadows():
    own = bge.logic.getCurrentController().owner
    if own['VolumetricShadows'] == True:
        bpy.context.scene.eevee.use_volumetric_shadows = True
    else:
        bpy.context.scene.eevee.use_volumetric_shadows = False
        bge.logic.getCurrentScene().resetTaaSamples = True

def VolumetricShadowSamples():
    own = bge.logic.getCurrentController().owner
    bpy.context.scene.eevee.volumetric_shadow_samples = int(own['VolumetricShadowSamples'])
    bge.logic.getCurrentScene().resetTaaSamples = True

def ShadowCubeSize():
    own = bge.logic.getCurrentController().owner
    ScubeS = ['64', '128', '256', '512', '1024', '2048', '4096']
    bpy.context.scene.eevee.shadow_cube_size = (ScubeS[(own['ShadowCubeSize'])])
    bge.logic.getCurrentScene().resetTaaSamples = True

def ShadowCascadeSize():
    own = bge.logic.getCurrentController().owner
    ScascadeS = ['64', '128', '256', '512', '1024', '2048', '4096']
    bpy.context.scene.eevee.shadow_cascade_size = (ScascadeS[(own['ShadowCascadeSize'])])
    bge.logic.getCurrentScene().resetTaaSamples = True
    
def HighQualityNormals():
    own = bge.logic.getCurrentController().owner
    if own['HighQualityNormals'] == True:
        bpy.context.scene.render.use_high_quality_normals = True
    else:
        bpy.context.scene.render.use_high_quality_normals = False
        bge.logic.getCurrentScene().resetTaaSamples = True

#def ShadowHighBitDepth():
#    own = bge.logic.getCurrentController().owner
#    if own['ShadowHighBitDepth'] == True:
#        bpy.context.scene.eevee.use_shadow_high_bitdepth = True
#    else:
#        bpy.context.scene.eevee.use_shadow_high_bitdepth = False
#        bge.logic.getCurrentScene().resetTaaSamples = True

#def SoftShadows():
#    own = bge.logic.getCurrentController().owner
#    if own['SoftShadows'] == True:
#        bpy.context.scene.eevee.use_soft_shadows = True
#    else:
#        bpy.context.scene.eevee.use_soft_shadows = False
#        bge.logic.getCurrentScene().resetTaaSamples = True

################################ BLENDER API END ################################
