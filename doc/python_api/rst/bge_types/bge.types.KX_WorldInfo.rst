KX_WorldInfo(EXP_PyObjectPlus)
==============================

base class --- :class:`EXP_PyObjectPlus`

.. class:: KX_WorldInfo(EXP_PyObjectPlus)

   A world object.

   .. code-block:: python

      # Set the mist color to red.
      import bge

      sce = bge.logic.getCurrentScene()

      sce.world.mistColor = [1.0, 0.0, 0.0]

   .. data:: KX_MIST_QUADRATIC

      Type of quadratic attenuation used to fade mist.

   .. data:: KX_MIST_LINEAR

      Type of linear attenuation used to fade mist.

   .. data:: KX_MIST_INV_QUADRATIC

      Type of inverse quadratic attenuation used to fade mist.

   .. attribute:: mistEnable

      Return the state of the mist.

      :type: bool

   .. attribute:: mistStart

      The mist start point.

      :type: float

   .. attribute:: mistDistance

      The mist distance fom the start point to reach 100% mist.

      :type: float

   .. attribute:: mistIntensity

      The mist intensity.

      :type: float

   .. attribute:: mistType

      The type of mist - must be KX_MIST_QUADRATIC, KX_MIST_LINEAR or KX_MIST_INV_QUADRATIC

   .. attribute:: mistColor

      The color of the mist. Black = [0.0, 0.0, 0.0], White = [1.0, 1.0, 1.0].
      Mist and background color sould always set to the same color.

      :type: :class:`mathutils.Color`

   .. attribute:: horizonColor

      The horizon color. Black = [0.0, 0.0, 0.0, 1.0], White = [1.0, 1.0, 1.0, 1.0].
      Mist and horizon color should always be set to the same color.

      :type: :class:`mathutils.Vector`

   .. attribute:: zenithColor

      The zenith color. Black = [0.0, 0.0, 0.0, 1.0], White = [1.0, 1.0, 1.0, 1.0].

      :type: :class:`mathutils.Vector`

   .. attribute:: ambientColor

      The color of the ambient light. Black = [0.0, 0.0, 0.0], White = [1.0, 1.0, 1.0].

      :type: :class:`mathutils.Color`

   .. attribute:: exposure

      Amount of exponential color correction for light.

      :type: float between 0.0 and 1.0 inclusive

   .. attribute:: range

      The color range that will be mapped to 0 - 1.

      :type: float between 0.2 and 5.0 inclusive

   .. attribute:: envLightEnergy

      The environment light energy.

      :type: float from 0.0 to infinite

   .. attribute:: envLightEnabled

      Returns True if Environment Lighting is enabled. Else returns False

      :type: bool (read only)

   .. attribute:: envLightColor

      White:       returns 0
      SkyColor:    returns 1
      SkyTexture:  returns 2

      :type: int (read only)
