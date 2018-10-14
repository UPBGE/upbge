BL_Texture(EXP_Value)
=====================

base class --- :class:`EXP_Value`

.. class:: BL_Texture(EXP_Value)

   A texture object that contains attributes of a material texture.

   .. attribute:: diffuseIntensity

      Amount texture affects diffuse reflectivity.

      :type: float

   .. attribute:: diffuseFactor

      Amount texture affects diffuse color.

      :type: float

   .. attribute:: alpha

      Amount texture affects alpha.

      :type: float

   .. attribute:: specularIntensity

      Amount texture affects specular reflectivity.

      :type: float

   .. attribute:: specularFactor

      Amount texture affects specular color.

      :type: float

   .. attribute:: hardness

      Amount texture affects hardness.

      :type: float

   .. attribute:: emit

      Amount texture affects emission.

      :type: float

   .. attribute:: mirror

      Amount texture affects mirror color.

      :type: float

   .. attribute:: normal

      Amount texture affects normal values.

      :type: float

   .. attribute:: parallaxBump

      Height of parallax occlusion mapping.

      :type: float

   .. attribute:: parallaxStep

      Number of steps to achieve parallax effect.

      :type: float

   .. attribute:: lodBias

      Amount bias on mipmapping.

      :type: float

   .. attribute:: bindCode

      Texture bind code/Id/number.

      :type: integer

   .. attribute:: renderer

      Texture renderer of this texture.

      :type: :class:`KX_CubeMap`, :class:`KX_PlanarMap` or None

   .. attribute:: ior

      Index Of Refraction used to compute refraction.

      :type: float (1.0 to 50.0)

   .. attribute:: refractionRatio

      Amount refraction mixed with reflection.

      :type: float (0.0 to 1.0)

   .. attribute:: uvOffset

      Offset applied to texture UV coordinates (mainly translation on U and V axis).

      :type: :class:`mathutils.Vector`

   .. attribute:: uvSize

      Scale applied to texture UV coordinates.

      :type: :class:`mathutils.Vector`

   .. attribute:: uvRotation

      Rotation applied to texture UV coordinates.

      :type: float (radians)
