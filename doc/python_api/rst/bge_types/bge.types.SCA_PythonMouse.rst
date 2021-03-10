SCA_PythonMouse(EXP_PyObjectPlus)
=================================

base class --- :class:`EXP_PyObjectPlus`

.. class:: SCA_PythonMouse(EXP_PyObjectPlus)

   The current mouse.

   .. attribute:: inputs

      A dictionary containing the input of each mouse event. (read-only).

      :type: dictionary {:ref:`keycode<mouse-keys>`::class:`SCA_InputEvent`, ...}

   .. attribute:: events

      a dictionary containing the status of each mouse event. (read-only).

      .. deprecated:: use :data:`inputs`

      :type: dictionary {:ref:`keycode<mouse-keys>`::ref:`status<input-status>`, ...}

   .. attribute:: activeInputs

      A dictionary containing the input of only the active mouse events. (read-only).

      :type: dictionary {:ref:`keycode<mouse-keys>`::class:`SCA_InputEvent`, ...}

   .. attribute:: active_events

      a dictionary containing the status of only the active mouse events. (read-only).

      .. deprecated:: use :data:`activeInputs`

      :type: dictionary {:ref:`keycode<mouse-keys>`::ref:`status<input-status>`, ...}
      
   .. attribute:: position

      The normalized x and y position of the mouse cursor.

      :type: tuple (x, y)

   .. attribute:: visible

      The visibility of the mouse cursor.
      
      :type: boolean
