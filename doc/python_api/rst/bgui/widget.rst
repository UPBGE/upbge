widget
======

This module defines the following constants:

Widget options:
* BGUI_DEFAULT = 0
* BGUI_CENTERX = 1
* BGUI_CENTERY = 2
* BGUI_NO_NORMALIZE = 4
* BGUI_NO_THEME = 8
* BGUI_NO_FOCUS = 16
* BGUI_CACHE = 32
* BGUI_CENTERED = BGUI_CENTERX | BGUI_CENTERY

Widget overflow:
* BGUI_OVERFLOW_NONE = 0
* BGUI_OVERFLOW_HIDDEN = 1
* BGUI_OVERFLOW_REPLACE = 2
* BGUI_OVERFLOW_CALLBACK = 3

Mouse event states:
* BGUI_MOUSE_NONE = 0
* BGUI_MOUSE_CLICK = 1
* BGUI_MOUSE_RELEASE = 2
* BGUI_MOUSE_ACTIVE = 4

.. note::

   The Widget class should not be used directly in a gui, but should instead be subclassed to create other widgets.

.. class:: bgui.widget.WeakMethod(f)

   Bases: :class:`object`

.. class:: bgui.widget.Animation(widget, attrib, value, time_, callback)

   Bases: :class:`object`

   .. method:: update()

.. class:: bgui.widget.ArrayAnimation(widget, attrib, value, time_, callback)

   Bases: :class:`bgui.widget.Animation`

   .. method:: update()

.. class:: bgui.widget.Widget(parent, name=None, aspect=None, size=[1, 1], pos=[0, 0], sub_theme='', options=0)

   Bases: :class:`object`

   The base widget class

   :arg parent: The widget’s parent
   :arg name: The name of the widget
   :arg aspect: Constrain the widget size to a specified aspect ratio
   :arg size: A tuple containing the width and height
   :arg pos: A tuple containing the x and y position
   :arg sub_theme: Name of a sub_theme defined in the theme file (similar to CSS classes)
   :arg options: Various other options


   .. attribute:: theme_section= 'Widget'

   .. attribute:: theme_options= {}

   .. attribute:: name= None

      The widget’s name

   .. attribute:: frozen= None

      Whether or not the widget should accept events

   .. attribute:: visible= None

      Whether or not the widget is visible

   .. attribute:: z_index= None

      The widget’s z-index. Widget’s with a higher z-index are drawn over those that have a lower z-index

   .. attribute:: on_click

      The widget’s on_click callback

   .. attribute:: on_release

      The widget’s on_release callback

   .. attribute:: on_hover

      The widget’s on_hover callback

   .. attribute:: on_mouse_enter

      The widget’s on_mouse_enter callback

   .. attribute:: on_mouse_exit

      The widget’s on_mouse_exit callback

   .. attribute:: on_active

      The widget’s on_active callback

   .. attribute:: parent

      The widget’s parent

   .. attribute:: system

      A reference to the system object

   .. attribute:: children

      The widget’s children

   .. attribute:: position

      The widget’s position

   .. attribute:: size

      The widget’s size

   .. method:: move(position, time, callback=None)

      Move a widget to a new position over a number of frames.

      :arg position: The new position
      :arg time: The time in milliseconds to take doing the move
      :arg callback: An optional callback that is called when he animation is complete

   .. method:: add_animation(animation)

      Add the animation to the list of currently running animations.

      :arg animation: The animation
