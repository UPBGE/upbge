KX_MaterialShader(KX_Shader)
============================

base class --- :class:`KX_Shader`

.. class:: KX_MaterialShader(KX_Shader)

   .. attribute:: objectCallbacks

      The list of python callbacks executed when the shader is used to render an object.
      All the functions can expect as argument the object currently rendered.

      .. code-block:: python

         def callback(object):
             print("render object %r" % object.name)

      :type: list of functions and/or methods

   .. attribute:: bindCallbacks

      The list of python callbacks executed when the shader is begin used to render.

      :type: list of functions and/or methods
