frame
=====

.. class:: bgui.frame.Frame(parent, name=None, border=None, aspect=None, size=[1, 1], pos=[0, 0], sub_theme='', options=0)

   Bases: :class:`~bgui.widget.Widget`

   Frame for storing other widgets

   :arg parent: The widget’s parent
   :arg name: The name of the widget
   :arg border: The size of the border around the frame (0 for no border)
   :arg aspect: Constrain the widget size to a specified aspect ratio
   :arg size: A tuple containing the width and height
   :arg pos: A tuple containing the x and y position
   :arg sub_theme: Name of a sub_theme defined in the theme file (similar to CSS classes)
   :arg options: Various other options


   .. attribute:: theme_section= 'Frame'

   .. attribute:: theme_options= {'Color3': (0, 0, 0, 0), 'Color4': (0, 0, 0, 0), 'BorderSize': 0, 'BorderColor': (0, 0, 0, 1), 'Color1': (0, 0, 0, 0), 'Color2': (0, 0, 0, 0)}

   .. attribute:: colors= None

      The colors for the four corners of the frame.

   .. attribute:: border_color= None

      The color of the border around the frame.
