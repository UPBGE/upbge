SCA_RaySensor(SCA_ISensor)
==========================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.SCA_ISensor`

.. class:: SCA_RaySensor

   A ray sensor detects the first object in a given direction.

   .. attribute:: propName

      The property the ray is looking for.

      :type: string

   .. attribute:: range

      The distance of the ray.

      :type: float

   .. attribute:: useMaterial

      Whether or not to look for a material (false = property).

      :type: boolean

   .. attribute:: useXRay

      Whether or not to use XRay.

      :type: boolean

   .. attribute:: mask

      The collision mask (16 layers mapped to a 16-bit integer) combined with each object's collision group, to hit only a subset of the
      objects in the scene. Only those objects for which ``collisionGroup & mask`` is true can be hit.

      :type: integer (bit mask)

   .. attribute:: hitObject

      The game object that was hit by the ray. (read-only).

      :type: :class:`~bge.types.KX_GameObject`

   .. attribute:: hitPosition

      The position (in worldcoordinates) where the object was hit by the ray. (read-only).

      :type: list [x, y, z]

   .. attribute:: hitNormal

      The normal (in worldcoordinates) of the object at the location where the object was hit by the ray. (read-only).

      :type: list [x, y, z]

   .. attribute:: hitMaterial

      The material of the object in the face hit by the ray. (read-only).

      :type: string

   .. attribute:: rayDirection

      The direction from the ray (in worldcoordinates). (read-only).

      :type: list [x, y, z]

   .. attribute:: axis

      The axis the ray is pointing on.

      :type: integer from 0 to 5

      * KX_RAY_AXIS_POS_X
      * KX_RAY_AXIS_POS_Y
      * KX_RAY_AXIS_POS_Z
      * KX_RAY_AXIS_NEG_X
      * KX_RAY_AXIS_NEG_Y
      * KX_RAY_AXIS_NEG_Z
