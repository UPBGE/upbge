image_button
============

.. class:: bgui.image_button.ImageButton(parent, name=None, default_image=None, default2_image=None, hover_image=None, click_image=None, aspect=None, size=[1, 1], pos=[0, 0], sub_theme='', options=0)

   Bases: :class:`~bgui.widget.Widget`

   A clickable image-based button.

   :arg parent: The widget’s parent
   :arg name: The name of the widget
   :arg default_image: List containing image data for the default state (‘image’, xcoord, ycoord, xsize, ysize)
   :arg default2_image: List containing image data for a second default state, which is used for toggling (‘image’, xcoord, ycoord, xsize, ysize)
   :arg hover_image: List containing image data for the hover state (‘image’, xcoord, ycoord, xsize, ysize)
   :arg click_image: List containing image data for the click state (‘image’, xcoord, ycoord, xsize, ysize)
   :arg aspect: Constrain the widget size to a specified aspect ratio
   :arg size: A tuple containing the width and height
   :arg pos: A tuple containing the x and y position
   :arg sub_theme: Name of a sub_theme defined in the theme file (similar to CSS classes)
   :arg options: Various other options

   .. attribute:: theme_section= 'ImageButton'

   .. attribute:: theme_options= {'Default2Image': (None, 0, 0, 1, 1), 'DefaultImage': (None, 0, 0, 1, 1), 'ClickImage': (None, 0, 0, 1, 1), 'HoverImage': (None, 0, 0, 1, 1)}

