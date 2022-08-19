SCA_PythonMouse(EXP_PyObjectPlus)
=================================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.EXP_PyObjectPlus`

.. class:: SCA_PythonMouse

   The current mouse.

   .. attribute:: inputs

      A dictionary containing the input of each mouse event. (read-only).

      :type: dict[:ref:`keycode<mouse-keys>`, :class:`~bge.types.SCA_InputEvent`]

   .. attribute:: events

      a dictionary containing the status of each mouse event. (read-only).

      .. deprecated:: 0.2.2

         Use :attr:`inputs`.

      :type: dict[:ref:`keycode<mouse-keys>`, :ref:`status<input-status>`]

   .. attribute:: activeInputs

      A dictionary containing the input of only the active mouse events. (read-only).

      :type: dict[:ref:`keycode<mouse-keys>`, :class:`~bge.types.SCA_InputEvent`]

   .. attribute:: active_events

      a dictionary containing the status of only the active mouse events. (read-only).

      .. deprecated:: 0.2.2

         Use :data:`activeInputs`.

      :type: dict[:ref:`keycode<mouse-keys>`, :ref:`status<input-status>`]

   .. attribute:: position

      The normalized x and y position of the mouse cursor.

      :type: tuple (x, y)

   .. attribute:: visible

      The visibility of the mouse cursor.

      :type: boolean
