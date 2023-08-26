SCA_MouseFocusSensor(SCA_MouseSensor)
=====================================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.SCA_MouseSensor`

.. class:: SCA_MouseFocusSensor

   The mouse focus sensor detects when the mouse is over the current game object.

   The mouse focus sensor works by transforming the mouse coordinates from 2d device
   space to 3d space then raycasting away from the camera.

   .. attribute:: raySource

      The worldspace source of the ray (the view position).

      :type: list (vector of 3 floats)

   .. attribute:: rayTarget

      The worldspace target of the ray.

      :type: list (vector of 3 floats)

   .. attribute:: rayDirection

      The :attr:`rayTarget` - :attr:`raySource` normalized.

      :type: list (normalized vector of 3 floats)

   .. attribute:: hitObject

      the last object the mouse was over.

      :type: :class:`~bge.types.KX_GameObject` or None

   .. attribute:: hitPosition

      The worldspace position of the ray intersection.

      :type: list (vector of 3 floats)

   .. attribute:: hitNormal

      the worldspace normal from the face at point of intersection.

      :type: list (normalized vector of 3 floats)

   .. attribute:: hitUV

      the UV coordinates at the point of intersection.

      :type: list (vector of 2 floats)

      If the object has no UV mapping, it returns [0, 0].

      The UV coordinates are not normalized, they can be < 0 or > 1 depending on the UV mapping.

   .. attribute:: usePulseFocus

      When enabled, moving the mouse over a different object generates a pulse. (only used when the 'Mouse Over Any' sensor option is set).

      :type: boolean

   .. attribute:: useXRay

      If enabled it allows the sensor to see through game objects that don't have the selected property or material.

     :type: boolean

   .. attribute:: mask

      The collision mask (16 layers mapped to a 16-bit integer) combined with each object's collision group, to hit only a subset of the
      objects in the scene. Only those objects for which ``collisionGroup & mask`` is true can be hit.

      :type: integer (bit mask)

   .. attribute:: propName

      The property or material the sensor is looking for.

     :type: string

   .. attribute:: useMaterial

      Determines if the sensor is looking for a property or material. KX_True = Find material; KX_False = Find property.

     :type: boolean
