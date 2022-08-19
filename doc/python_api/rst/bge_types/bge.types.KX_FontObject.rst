KX_FontObject(KX_GameObject)
============================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.KX_GameObject`

.. class:: KX_FontObject

   A Font game object.

   It is possible to use attributes from :type: :class:`~bpy.types.TextCurve`

   .. code-block:: python

      import bge

      # Use bge module to get/set game property + transform
      font_object = (bge.logic.getCurrentController()).owner
      font_object["Text"] = "Text Example"
      font_object.worldPosition = [-2.5, 1.0, 0.0]

      # Use bpy.types.TextCurve attributes to set other text settings
      font_object_text = font_object.blenderObject.data
      font_object_text.size = 1
      font_object_text.resolution_u = 4
      font_object_text.align_x = "LEFT"
