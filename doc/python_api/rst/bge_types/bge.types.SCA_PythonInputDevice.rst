SCA_PythonInputDevice(EXP_PyObjectPlus)
=======================================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.EXP_PyObjectPlus`

.. class:: SCA_PythonInputDevice

   A Python interface to the raw values of a joystick-class input device.

   Unlike :class:`~bge.types.SCA_PythonJoystick`, which only handles devices recognized as
   gamepads and maps them to an Xbox controller layout, this class exposes every axis, button
   and hat reported by the device. This covers flight sticks, throttles, HOTAS, steering wheels,
   pedals, yokes, button boxes and gamepads. Keyboards and mice are not included, use
   :data:`bge.logic.keyboard` and :data:`bge.logic.mouse` instead.

   Devices recognized as gamepads are available both here and in :data:`bge.logic.joysticks`,
   see :attr:`isGamepad`.

   Instances are stored in :data:`bge.logic.inputDevices`. A device keeps the same instance
   while it stays connected. When it is disconnected the instance becomes invalid and its slot
   is set to None.

   .. note::

      Slots are assigned in connection order, so the same device can end up in a different slot
      between sessions. Identify devices by :attr:`name` or :attr:`guid` instead of by slot.

   .. code-block:: python

      import bge

      def find_device(name):
          for device in bge.logic.inputDevices:
              if device and name in device.name:
                  return device
          return None

      stick = find_device("T.16000M")
      if stick:
          roll, pitch, twist = stick.getAxis(0), stick.getAxis(1), stick.getAxis(2)
          firing = stick.getButton(0)

   .. attribute:: name

      The name of the device as reported by the driver, empty if unknown. (read-only)

      :type: string

   .. attribute:: guid

      The SDL GUID of the device as 32 hexadecimal characters. (read-only)

      It identifies the device model and stays the same across sessions and slots, which makes
      it suitable to store per-device bindings. Two identical devices share the same GUID.

      :type: string

   .. attribute:: vendorId

      The USB vendor ID of the device, 0 if unavailable. (read-only)

      :type: integer

   .. attribute:: productId

      The USB product ID of the device, 0 if unavailable. (read-only)

      :type: integer

   .. attribute:: deviceType

      The category of the device as reported by the driver. (read-only)

      Many drivers do not report a category, in which case the value is ``'unknown'``.

      :type: string in ['unknown', 'gamepad', 'wheel', 'arcade_stick', 'flight_stick', 'dance_pad', 'guitar', 'drum_kit', 'arcade_pad', 'throttle']

   .. attribute:: isGamepad

      True if the device is recognized as a gamepad, in which case it is also available,
      with mapped values, in :data:`bge.logic.joysticks`. (read-only)

      :type: boolean

   .. attribute:: index

      The slot of the device in :data:`bge.logic.inputDevices`. (read-only)

      :type: integer

   .. attribute:: numAxes

      The number of axes of the device. (read-only)

      :type: integer

   .. attribute:: numButtons

      The number of buttons of the device. (read-only)

      :type: integer

   .. attribute:: numHats

      The number of hats (POV switches) of the device. (read-only)

      :type: integer

   .. attribute:: axisValues

      The position of all axes, :attr:`numAxes` long. (read-only)

      Each value is between -1.0 and 1.0. The meaning and order of the axes depend on the
      device; for example a flight stick usually reports roll, pitch, twist and a throttle slider.
      Throttles, triggers and pedals that rest at one end report -1.0 or 1.0 in that position,
      not 0.0.

      :type: list of floats

   .. attribute:: buttonValues

      The pressed state of all buttons, :attr:`numButtons` long. (read-only)

      :type: list of booleans

   .. attribute:: activeButtons

      The indices of the pressed buttons. (read-only)

      :type: list of integers

   .. attribute:: hatValues

      The position of all hats, :attr:`numHats` long. (read-only)

      Each value is a bitmask of the following directions, diagonals combine two of them:

      * centered: 0
      * up: 1
      * right: 2
      * down: 4
      * left: 8
      * up-right: 3, down-right: 6, down-left: 12, up-left: 9

      :type: list of integers

   .. method:: getAxis(index)

      Return the position of an axis, same as ``axisValues[index]`` without building a list.

      :arg index: The axis index, between 0 and :attr:`numAxes` - 1.
      :type index: integer
      :return: The axis position, between -1.0 and 1.0.
      :rtype: float
      :raises IndexError: If the index is out of range.

   .. method:: getButton(index)

      Return the state of a button, same as ``buttonValues[index]`` without building a list.

      :arg index: The button index, between 0 and :attr:`numButtons` - 1.
      :type index: integer
      :return: True if the button is pressed.
      :rtype: boolean
      :raises IndexError: If the index is out of range.

   .. method:: getHat(index)

      Return the position of a hat, same as ``hatValues[index]`` without building a list.

      :arg index: The hat index, between 0 and :attr:`numHats` - 1.
      :type index: integer
      :return: The hat direction bitmask, see :attr:`hatValues`.
      :rtype: integer
      :raises IndexError: If the index is out of range.
