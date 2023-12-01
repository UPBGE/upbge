SCA_MouseSensor(SCA_ISensor)
============================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.SCA_ISensor`

.. class:: SCA_MouseSensor

   Mouse Sensor logic brick.

   .. attribute:: position

      current [x, y] coordinates of the mouse, in frame coordinates (pixels).

      :type: [integer, integer]

   .. attribute:: mode

      sensor mode. one of the following constants:

      * KX_MOUSESENSORMODE_LEFTBUTTON(1)
      * KX_MOUSESENSORMODE_MIDDLEBUTTON(2)
      * KX_MOUSESENSORMODE_RIGHTBUTTON(3)
      * KX_MOUSESENSORMODE_BUTTON4(4)
      * KX_MOUSESENSORMODE_BUTTON5(5)
      * KX_MOUSESENSORMODE_BUTTON6(6)
      * KX_MOUSESENSORMODE_BUTTON7(7)
      * KX_MOUSESENSORMODE_WHEELUP(8)
      * KX_MOUSESENSORMODE_WHEELDOWN(9)
      * KX_MOUSESENSORMODE_MOVEMENT(10)

      :type: integer

   .. method:: getButtonStatus(button)

      Get the mouse button status.

      :arg button: The code that represents the key you want to get the state of, use one of :ref:`these constants<mouse-keys>`
      :type button: int
      :return: The state of the given key, can be one of :ref:`these constants<input-status>`
      :rtype: int
