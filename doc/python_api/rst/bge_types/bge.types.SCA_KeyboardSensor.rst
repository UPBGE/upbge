SCA_KeyboardSensor(SCA_ISensor)
===============================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.SCA_ISensor`

.. class:: SCA_KeyboardSensor

   A keyboard sensor detects player key presses.

   See module :mod:`bge.events` for keycode values.

   .. attribute:: key

      The key code this sensor is looking for. Expects a keycode from :mod:`bge.events` module.

      :type: integer

   .. attribute:: hold1

      The key code for the first modifier this sensor is looking for. Expects a keycode from :mod:`bge.events` module.

      :type: integer

   .. attribute:: hold2

      The key code for the second modifier this sensor is looking for. Expects a keycode from :mod:`bge.events` module.

      :type: integer

   .. attribute:: toggleProperty

      The name of the property that indicates whether or not to log keystrokes as a string.

      :type: string

   .. attribute:: targetProperty

      The name of the property that receives keystrokes in case in case a string is logged.

      :type: string

   .. attribute:: useAllKeys

      Flag to determine whether or not to accept all keys.

      :type: boolean

   .. attribute:: inputs

      A list of pressed input keys that have either been pressed, or just released, or are active this frame. (read-only).

      :type: dict[:ref:`keycode<keyboard-keys>`, :class:`~bge.types.SCA_InputEvent`]

   .. attribute:: events

      a list of pressed keys that have either been pressed, or just released, or are active this frame. (read-only).

      .. deprecated:: 0.2.2

         Use :data:`inputs`

      :type: list [[:ref:`keycode<keyboard-keys>`, :ref:`status<input-status>`], ...]

   .. method:: getKeyStatus(keycode)

      Get the status of a key.

      :arg keycode: The code that represents the key you want to get the state of, use one of :ref:`these constants<keyboard-keys>`
      :type keycode: integer
      :return: The state of the given key, can be one of :ref:`these constants<input-status>`
      :rtype: int
