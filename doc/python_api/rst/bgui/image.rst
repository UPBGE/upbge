image
=====

.. class:: bgui.image.Image(parent, img, name=None, aspect=None, size=[1, 1], pos=[0, 0], texco=[(0, 0), (1, 0), (1, 1), (0, 1)], sub_theme='', options=0)

   Bases: :class:`~bgui.widget.Widget`

   Widget for displaying images

   :arg parent: The widget’s parent
   :arg name: The name of the widget
   :arg img: The image to use for the widget
   :arg aspect: Constrain the widget size to a specified aspect ratio
   :arg size: A tuple containing the width and height
   :arg pos: A tuple containing the x and y position
   :arg texco: The UV texture coordinates to use for the image
   :arg sub_theme: Name of a sub_theme defined in the theme file (similar to CSS classes)
   :arg options: Various other options


   .. attribute:: texco= None

      The UV texture coordinates to use for the image.

   .. attribute:: color= None

      The color of the plane the texture is on.

   .. attribute:: image_size

      The size (in pixels) of the currently loaded image, or [0, 0] if an image is not loaded.

   .. method:: update_image(img)

      Changes the image texture.

      :arg img: The path to the new image.

      :return type: None.
