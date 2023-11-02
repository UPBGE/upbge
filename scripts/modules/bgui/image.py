# SPDX-License-Identifier: MIT
# Copyright 2010-2011 Mitchell Stokes

# <pep8 compliant>

import bpy
import gpu
from gpu_extras.batch import batch_for_shader

from .widget import Widget, BGUI_DEFAULT, BGUI_CACHE


class Image(Widget):
  """Widget for displaying images"""

  def __init__(self, parent, img, name=None, aspect=None, size=[1, 1], pos=[0, 0],
        texco=[(0, 0), (1, 0), (1, 1), (0, 1)], sub_theme='', options=BGUI_DEFAULT):
    """:param parent: the widget's parent
    :param name: the name of the widget
    :param img: the image to use for the widget
    :param aspect: constrain the widget size to a specified aspect ratio
    :param size: a tuple containing the width and height
    :param pos: a tuple containing the x and y position
    :param texco: the UV texture coordinates to use for the image
    :param sub_theme: name of a sub_theme defined in the theme file (similar to CSS classes)
    :param options: various other options
    """

    Widget.__init__(self, parent, name, aspect, size, pos, sub_theme, options)

    if img != None:
      self.image = bpy.data.images.load(img)
      self.texture = gpu.texture.from_image(self.image)
    else:
      self.texture = None

    #: The UV texture coordinates to use for the image.
    self.texco = texco
    self.width, self.height = self.image.size

    # The shader.
    self.shader = gpu.shader.from_builtin('IMAGE')

  @property
  def image_size(self):
    """The size (in pixels) of the currently loaded image, or [0, 0] if an image is not loaded"""
    return self.image.size

  def update_image(self, img):
    """Changes the image texture

    :param img: the path to the new image
    :rtype: None
    """
    self.image = bpy.data.images.load(img)
    self.texture = gpu.texture.from_image(self.image)

  def _draw(self):
    """Draws the image"""

    # Enable alpha blending
    gpu.state.blend_set('ALPHA_PREMULT')

    vertices = self.gpu_view_position
    indices  = ((0, 1, 3), (3, 1, 2))
    self.batch = batch_for_shader(self.shader, 'TRIS', {"pos": vertices, "texCoord": self.texco}, indices=indices)

    self.shader.bind()
    self.shader.uniform_sampler("image", self.texture)
    self.batch.draw(self.shader)

    gpu.state.blend_set('NONE')

    # Now draw the children
    Widget._draw(self)
