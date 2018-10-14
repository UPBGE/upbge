SCA_InputEvent(EXP_PyObjectPlus)
================================

base class --- :class:`EXP_PyObjectPlus`

.. class:: SCA_InputEvent(EXP_PyObjectPlus)

   Events for a keyboard or mouse input.

   .. attribute:: status

      A list of existing status of the input from the last frame.
      Can contain :data:`bge.logic.KX_INPUT_NONE` and :data:`bge.logic.KX_INPUT_ACTIVE`.
      The list always contains one value.
      The first value of the list is the last value of the list in the last frame. (read-only)

      :type: list of integer.

   .. attribute:: queue

      A list of existing events of the input from the last frame.
      Can contain :data:`bge.logic.KX_INPUT_JUST_ACTIVATED` and :data:`bge.logic.KX_INPUT_JUST_RELEASED`.
      The list can be empty. (read-only)

      :type: list of integer.

   .. attribute:: values

      A list of existing value of the input from the last frame.
      For keyboard it contains 1 or 0 and for mouse the coordinate of the mouse or the movement of the wheel mouse.
      The list contains always one value, the size of the list is the same than :data:`queue` + 1 only for keyboard inputs.
      The first value of the list is the last value of the list in the last frame. (read-only)

      Example to get the non-normalized mouse coordinates:

      .. code-block:: python

         import bge

         x = bge.logic.mouse.inputs[bge.events.MOUSEX].values[-1]
         y = bge.logic.mouse.inputs[bge.events.MOUSEY].values[-1]

         print("Mouse non-normalized position: x: {0}, y: {1}".format(x, y))

      :type: list of integer.

   .. attribute:: inactive

      True if the input was inactive from the last frame.

      :type: boolean

   .. attribute:: active

      True if the input was active from the last frame.

      :type: boolean

   .. attribute:: activated

      True if the input was activated from the last frame.

      :type: boolean

   .. attribute:: released

      True if the input was released from the last frame.

      :type: boolean

   .. attribute:: type

      The type of the input.
      One of :ref:`these constants<keyboard-keys>`

      :type: integer
