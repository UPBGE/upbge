SCA_ActuatorSensor(SCA_ISensor)
===============================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.SCA_ISensor`

.. class:: SCA_ActuatorSensor

   Actuator sensor detect change in actuator state of the parent object.
   It generates a positive pulse if the corresponding actuator is activated
   and a negative pulse if the actuator is deactivated.

   .. attribute:: actuator

      the name of the actuator that the sensor is monitoring.

      :type: string
