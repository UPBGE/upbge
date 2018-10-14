SCA_PythonMouse(EXP_PyObjectPlus)
=================================

base class --- :class:`EXP_PyObjectPlus`

.. class:: SCA_PythonMouse(EXP_PyObjectPlus)

   The current mouse.

   .. warning::

      Mouse normalization is using the maximum coordinate in width and height, whether `bge.render.getWindowWidth() - 1` or `bge.render.getWindowHeight() - 1`.

      A script setting the mouse at center is similar to the following example:

      .. code-block:: python

         import bge

         w, h = bge.render.getWindowWidth() - 1, bge.render.getWindowHeight() - 1
         center_x, center_y = (w // 2) / w, (h // 2) / h

         bge.logic.mouse.position = center_x, center_y

      Note the usage of floor division to round an existing coordinate.

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
