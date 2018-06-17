KX_NearSensor(KX_CollisionSensor)
=================================

base class --- :class:`KX_CollisionSensor`

.. class:: KX_NearSensor(KX_CollisionSensor)

   A near sensor is a specialised form of touch sensor.

   .. attribute:: distance

      The near sensor activates when an object is within this distance.

      :type: float

   .. attribute:: resetDistance

      The near sensor deactivates when the object exceeds this distance.

      :type: float
