
Rasterizer (bge.render)
=======================

*****
Intro
*****

.. module:: bge.render

Example of using a :class:`bge.types.SCA_MouseSensor`,
and two :class:`bge.types.SCA_ObjectActuator` to implement MouseLook:

.. note::
   This can also be achieved with the :class:`bge.types.SCA_MouseActuator`.

.. code-block:: python

   # To use a mouse movement sensor "Mouse" and a
   # motion actuator to mouse look:
   import bge

   # scale sets the speed of motion
   scale = 1.0, 0.5

   co = bge.logic.getCurrentController()
   obj = co.owner
   mouse = co.sensors["Mouse"]
   lmotion = co.actuators["LMove"]
   wmotion = co.actuators["WMove"]

   # Transform the mouse coordinates to see how far the mouse has moved.
   def mousePos():
      x = (bge.render.getWindowWidth() / 2 - mouse.position[0]) * scale[0]
      y = (bge.render.getWindowHeight() / 2 - mouse.position[1]) * scale[1]
      return (x, y)

   pos = mousePos()

   # Set the amount of motion: X is applied in world coordinates...
   wmotion.useLocalTorque = False
   wmotion.torque = ((0.0, 0.0, pos[0]))

   # ...Y is applied in local coordinates
   lmotion.useLocalTorque = True
   lmotion.torque = ((-pos[1], 0.0, 0.0))

   # Activate both actuators
   co.activate(lmotion)
   co.activate(wmotion)

   # Centre the mouse
   bge.render.setMousePosition(int(bge.render.getWindowWidth() / 2), int(bge.render.getWindowHeight() / 2))

*********
Constants
*********

.. data:: KX_TEXFACE_MATERIAL

   .. deprecated:: 0.2.2

   :type: integer

.. data:: KX_BLENDER_MULTITEX_MATERIAL

   .. deprecated:: 0.2.2

   :type: integer

.. data:: KX_BLENDER_GLSL_MATERIAL

   .. deprecated:: 0.2.2

   :type: integer

.. data:: VSYNC_OFF

   Disables vsync

   :type: integer

.. data:: VSYNC_ON

   Enables vsync

   :type: integer

.. data:: VSYNC_ADAPTIVE

   Enables adaptive vsync if supported.
   Adaptive vsync enables vsync if the framerate is above the monitors refresh rate.
   Otherwise, vsync is disabled if the framerate is too low.

   :type: integer

.. data:: LEFT_EYE

   .. deprecated:: 0.3.0

   Left eye being used during stereoscopic rendering.

   :type: integer

.. data:: RIGHT_EYE

   .. deprecated:: 0.3.0

   Right eye being used during stereoscopic rendering.

   :type: integer

.. data:: RAS_MIPMAP_NONE

   .. deprecated:: 0.3.0

   Disables Mipmap filtering.

   :type: integer

.. data:: RAS_MIPMAP_NEAREST

   .. deprecated:: 0.3.0

   Applies mipmap filtering with nearest neighbour interpolation.

   :type: integer

.. data:: RAS_MIPMAP_LINEAR

   .. deprecated:: 0.3.0

   Applies mipmap filtering with nearest linear interpolation.

   :type: integer

*********
Functions
*********

.. function:: getWindowWidth()

   Gets the width of the window (in pixels)

   :rtype: integer

.. function:: getWindowHeight()

   Gets the height of the window (in pixels)

   :rtype: integer

.. function:: setWindowSize(width, height)

   Set the width and height of the window (in pixels). This also works for fullscreen applications.

   .. note:: Only works in the standalone player, not the Blender-embedded player.

   :arg width: width in pixels
   :type width: integer
   :arg height: height in pixels
   :type height: integer

.. function:: setFullScreen(enable)

   Set whether or not the window should be fullscreen.

   .. note:: Only works in the standalone player, not the Blender-embedded player.

   :arg enable: ``True`` to set full screen, ``False`` to set windowed.
   :type enable: bool

.. function:: getFullScreen()

   Returns whether or not the window is fullscreen.

   .. note:: Only works in the standalone player, not the Blender-embedded player; there it always returns False.

   :rtype: bool

.. function:: getDisplayDimensions()

   Get the display dimensions, in pixels, of the display (e.g., the
   monitor). Can return the size of the entire view, so the
   combination of all monitors; for example, ``(3840, 1080)`` for two
   side-by-side 1080p monitors.
   
   :rtype: tuple (width, height)

.. function:: makeScreenshot(filename)

   Writes an image file with the displayed image at the frame end.

   The image is written to *'filename'*.
   The path may be absolute (eg. ``/home/foo/image``) or relative when started with
   ``//`` (eg. ``//image``). Note that absolute paths are not portable between platforms.
   If the filename contains a ``#``,
   it will be replaced by an incremental index so that screenshots can be taken multiple
   times without overwriting the previous ones (eg. ``image-#``).

   Settings for the image are taken from the render settings (file format and respective settings,
   gamma and colospace conversion, etc).
   The image resolution matches the framebuffer, meaning, the window size and aspect ratio.
   When running from the standalone player, instead of the embedded player, only PNG files are supported.
   Additional color conversions are also not supported.

   :arg filename: path and name of the file to write
   :type filename: string


.. function:: enableVisibility(visible)

   .. deprecated:: 0.0.1

      Doesn't do anything.


.. function:: showMouse(visible)

   Enables or disables the operating system mouse cursor.

   :arg visible:
   :type visible: boolean


.. function:: setMousePosition(x, y)

   Sets the mouse cursor position.

   :arg x: X-coordinate in screen pixel coordinates.
   :type x: integer
   :arg y: Y-coordinate in screen pixel coordinates.
   :type y: integer


.. function:: setBackgroundColor(rgba)

   .. deprecated:: 0.2.2

      Use :attr:`bge.texture.ImageRender.horizon` or :attr:`bge.texture.ImageRender.zenith` instead.


.. function:: setEyeSeparation(eyesep)

   .. deprecated:: 0.3.0

   Sets the eye separation for stereo mode. Usually Focal Length/30 provides a comfortable value.

   :arg eyesep: The distance between the left and right eye.
   :type eyesep: float


.. function:: getEyeSeparation()

   .. deprecated:: 0.3.0

   Gets the current eye separation for stereo mode.

   :rtype: float


.. function:: setFocalLength(focallength)

   .. deprecated:: 0.3.0

   Sets the focal length for stereo mode. It uses the current camera focal length as initial value.

   :arg focallength: The focal length.
   :type focallength: float

.. function:: getFocalLength()

   .. deprecated:: 0.3.0

   Gets the current focal length for stereo mode.

   :rtype: float

.. function:: getStereoEye()

   .. deprecated:: 0.3.0

   Gets the current stereoscopy eye being rendered.
   This function is mainly used in a :attr:`bge.types.KX_Scene.pre_draw` callback
   function to customize the camera projection matrices for each
   stereoscopic eye.

   :return: One of :data:`~bge.render.LEFT_EYE`, :data:`~bge.render.RIGHT_EYE`.
   :rtype: LEFT_EYE, RIGHT_EYE

.. function:: setMaterialMode(mode)

   .. deprecated:: 0.2.2

.. function:: getMaterialMode(mode)

   .. deprecated:: 0.2.2

.. function:: setGLSLMaterialSetting(setting, enable)

   .. deprecated:: 0.3.0

.. function:: getGLSLMaterialSetting(setting)

   .. deprecated:: 0.3.0

.. function:: setAnisotropicFiltering(level)

   .. deprecated:: 0.3.0

   Set the anisotropic filtering level for textures.

   :arg level: The new anisotropic filtering level to use
   :type level: integer (must be one of 1, 2, 4, 8, 16)

   .. note:: Changing this value can cause all textures to be recreated, which can be slow.

.. function:: getAnisotropicFiltering()

   .. deprecated:: 0.3.0

   Get the anisotropic filtering level used for textures.

   :rtype: integer (one of 1, 2, 4, 8, 16)

.. function:: setMipmapping(value)

   .. deprecated:: 0.3.0

   Change how to use mipmapping.

   :arg value: One of :data:`~bge.render.RAS_MIPMAP_NONE`, :data:`~bge.render.RAS_MIPMAP_NEAREST`, :data:`~bge.render.RAS_MIPMAP_LINEAR`
   :type value: integer

   .. note:: Changing this value can cause all textures to be recreated, which can be slow.

.. function:: getMipmapping()

   .. deprecated:: 0.3.0

   Get the current mipmapping setting.

   :return: One of :data:`~bge.render.RAS_MIPMAP_NONE`, :data:`~bge.render.RAS_MIPMAP_NEAREST`, :data:`~bge.render.RAS_MIPMAP_LINEAR`
   :rtype: integer

.. function:: drawLine(fromVec,toVec,color)

   Draw a line in the 3D scene.

   :arg fromVec: the origin of the line
   :type fromVec: list [x, y, z]
   :arg toVec: the end of the line
   :type toVec: list [x, y, z]
   :arg color: the color of the line
   :type color: list [r, g, b, a]


.. function:: enableMotionBlur(factor)

   .. deprecated:: 0.3.0

   Enable the motion blur effect.

   :arg factor: the amount of motion blur to display.
   :type factor: float [0.0 - 1.0]


.. function:: disableMotionBlur()

   .. deprecated:: 0.3.0

   Disable the motion blur effect.

.. function:: showFramerate(enable)

   Show or hide the framerate.

   :arg enable:
   :type enable: boolean

.. function:: showProfile(enable)

   Show or hide the profile.

   :arg enable:
   :type enable: boolean

.. function:: showProperties(enable)

   Show or hide the debug properties.

   :arg enable:
   :type enable: boolean

.. function:: autoDebugList(enable)

   Enable or disable auto adding debug properties to the debug list.

   :arg enable:
   :type enable: boolean

.. function:: clearDebugList()

   Clears the debug property list.

.. function:: setVsync(value)

   Set the vsync value

   :arg value: One of :data:`~bge.render.VSYNC_OFF`, :data:`~bge.render.VSYNC_ON`, :data:`~bge.render.VSYNC_ADAPTIVE`
   :type value: integer

.. function:: getVsync()

   Get the current vsync value

   :return: One of :data:`~bge.render.VSYNC_OFF`, :data:`~bge.render.VSYNC_ON`, :data:`~bge.render.VSYNC_ADAPTIVE`
   :rtype: integer

