SCA_PythonJoystick(EXP_PyObjectPlus)
====================================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.EXP_PyObjectPlus`

.. class:: SCA_PythonJoystick

   A Python interface to a joystick.

   .. attribute:: name

      The name assigned to the joystick by the operating system. (read-only)

      :type: string

   .. attribute:: activeButtons

      A list of active button values. (read-only)

      :type: list

   .. attribute:: axisValues

      The state of the joysticks axis as a list of values :attr:`numAxis` long. (read-only).

      :type: list of ints.

      Each specifying the value of an axis between -1.0 and 1.0
      depending on how far the axis is pushed, 0 for nothing.
      The first 2 values are used by most joysticks and gamepads for directional control.
      3rd and 4th values are only on some joysticks and can be used for arbitrary controls.

      * left:[-1.0, 0.0, ...]
      * right:[1.0, 0.0, ...]
      * up:[0.0, -1.0, ...]
      * down:[0.0, 1.0, ...]

   .. attribute:: hatValues

      .. deprecated:: 0.2.2

         Use :attr:`activeButtons` instead.

   .. attribute:: numAxis

      The number of axes for the joystick at this index. (read-only).

      :type: integer

   .. attribute:: numButtons

      The number of buttons for the joystick at this index. (read-only).

      :type: integer

   .. attribute:: numHats

      .. deprecated:: 0.2.2

         Use :attr:`numButtons` instead.

   .. method:: startVibration()

      Starts the vibration.

      :return: None

   .. method:: stopVibration()

      Stops the vibration.

      :return: None

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
