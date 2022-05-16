progress_bar
============

.. class:: bgui.progress_bar.ProgressBar(parent, name=None, percent=1.0, sub_theme='', aspect=None, size=[1, 1], pos=[0, 0], options=0)

   Bases: :class:`~bgui.widget.Widget`

   A solid progress bar. Controlled via the ‘percent’ property which assumes percent as a 0-1 floating point number.

   :arg parent: The widget’s parent
   :arg name: The name of the widget
   :arg percent: The initial percent
   :arg sub_theme: Name of a sub_theme defined in the theme file (similar to CSS classes)
   :arg aspect: Constrain the widget size to a specified aspect ratio
   :arg size: A tuple containing the width and height
   :arg pos: A tuple containing the x and y position
   :arg options: Various other options

   .. attribute:: theme_section= 'ProgressBar'

   .. attribute:: theme_options= {'FillColor3': (0.0, 0.42, 0.02, 1.0), 'BGColor4': (0, 0, 0, 1), 'BorderSize': 1, 'FillColor4': (0.0, 0.42, 0.02, 1.0), 'BGColor3': (0, 0, 0, 1), 'BGColor1': (0, 0, 0, 1), 'FillColor2': (0.0, 0.42, 0.02, 1.0), 'FillColor1': (0.0, 0.42, 0.02, 1.0), 'BGColor2': (0, 0, 0, 1), 'BorderColor': (0, 0, 0, 1)}

   .. attribute:: percent
