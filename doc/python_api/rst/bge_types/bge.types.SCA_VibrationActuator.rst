SCA_VibrationActuator(SCA_IActuator)
====================================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.SCA_IActuator`

.. class:: SCA_VibrationActuator

   Vibration Actuator.

   .. attribute:: joyindex

      Joystick index.

      :type: integer (0 to 7)

   .. attribute:: strengthLeft

      Strength of the Low frequency joystick's motor (placed at left position usually).

      :type: float (0.0 to 1.0)

   .. attribute:: strengthRight

      Strength of the High frequency joystick's motor (placed at right position usually).

      :type: float (0.0 to 1.0)

   .. attribute:: duration

      Duration of the vibration in milliseconds.

      :type: integer (0 to infinite)

   .. attribute:: isVibrating

      Check status of joystick vibration

      :type: bool (true vibrating and false stopped)

   .. attribute:: hasVibration

      Check if the joystick supports vibration

      :type: bool (true supported and false not supported)

   .. method:: startVibration()

      Starts the vibration.

      :return: None

   .. method:: stopVibration()

      Stops the vibration.

      :return: None


