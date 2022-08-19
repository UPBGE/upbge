KX_LightObject(KX_GameObject)
=============================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.KX_GameObject`

.. class:: KX_LightObject

   A Light game object.

   It is possible to use attributes from :type: :class:`~bpy.types.Light`

   .. code-block:: python

      import bge

      # Use bge module to get/set game property + transform
      kxlight = (bge.logic.getCurrentController()).owner
      kxlight["Text"] = "Text Example"
      kxlight.worldPosition = [-2.5, 1.0, 0.0]

      # Use bpy.types.Light attributes to set other light settings
      lightData = kxlight.blenderObject.data
      lightData.energy = 1000.0
      lightData.color = [1.0, 0.0, 0.0]
      lightData.type = "POINT"

