SCA_CameraActuator(SCA_IActuator)
=================================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.SCA_IActuator`

.. class:: SCA_CameraActuator

   Applies changes to a camera.

   .. attribute:: damping

      strength of of the camera following movement.

      :type: float

   .. attribute:: axis

      The camera axis (0, 1, 2) for positive ``XYZ``, (3, 4, 5) for negative ``XYZ``.

      :type: int

   .. attribute:: min

      minimum distance to the target object maintained by the actuator.

      :type: float

   .. attribute:: max

      maximum distance to stay from the target object.

      :type: float

   .. attribute:: height

      height to stay above the target object.

      :type: float

   .. attribute:: object

      the object this actuator tracks.

      :type: :class:`~bge.types.KX_GameObject` or None
