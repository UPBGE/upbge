SCA_InputEvent(PyObjectPlus)
============================

.. module:: bge.types

base class --- :class:`PyObjectPlus`

.. class:: SCA_InputEvent(PyObjectPlus)

   Events for a keyboard or mouse input.

   For compatibility the user can still use `==` operator to this class, same as `if event`.
   The both behaviours raise a deprecation message.
   This compatibility obligate the user to be very explicit when comparing or testing this class.
   For example to test if a event variable is not `None` the only way is:

   .. code-block:: python

      import bge

      event = None
      if some_test:
          event = bge.logic.keyboard.events[bge.events.AKEY]
      elif some_other_test:
          event = bge.logic.keyboard.events[bge.events.BKEY]

      # The user must use "is not None".
      if event is not None:
          do_something()


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

         x = bge.logic.mouse.events[bge.events.MOUSEX].values[-1]
         y = bge.logic.mouse.events[bge.events.MOUSEY].values[-1]

         print("Mouse non-normalized position: x: {0}, y: {1}".format(x, y))

      :type: list of integer.

   .. attribute:: type

      The type of the input.
      One of :ref:`these constants<keyboard-keys>`

      :type: integer
