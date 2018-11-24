KX_Scene(EXP_PyObjectPlus)
==========================

base class --- :class:`EXP_PyObjectPlus`

.. class:: KX_Scene(EXP_PyObjectPlus)

   An active scene that gives access to objects, cameras, lights and scene attributes.

   The activity culling stuff is supposed to disable logic bricks when their owner gets too far
   from the active camera.  It was taken from some code lurking at the back of KX_Scene - who knows
   what it does!

   .. code-block:: python

      from bge import logic

      # get the scene
      scene = logic.getCurrentScene()

      # print all the objects in the scene
      for object in scene.objects:
         print(object.name)

      # get an object named 'Cube'
      object = scene.objects["Cube"]

      # get the first object in the scene.
      object = scene.objects[0]

   .. code-block:: python

      # Get the depth of an object in the camera view.
      from bge import logic

      object = logic.getCurrentController().owner
      cam = logic.getCurrentScene().active_camera

      # Depth is negative and decreasing further from the camera
      depth = object.position[0]*cam.world_to_camera[2][0] + object.position[1]*cam.world_to_camera[2][1] + object.position[2]*cam.world_to_camera[2][2] + cam.world_to_camera[2][3]

   @bug: All attributes are read only at the moment.

   .. attribute:: name

      The scene's name, (read-only).

      :type: string

   .. attribute:: objects

      A list of objects in the scene, (read-only).

      :type: :class:`EXP_ListValue` of :class:`KX_GameObject`

   .. attribute:: objectsInactive

      A list of objects on background layers (used for the addObject actuator), (read-only).

      :type: :class:`EXP_ListValue` of :class:`KX_GameObject`

   .. attribute:: lights

      A list of lights in the scene, (read-only).

      :type: :class:`EXP_ListValue` of :class:`KX_LightObject`

   .. attribute:: cameras

      A list of cameras in the scene, (read-only).

      :type: :class:`EXP_ListValue` of :class:`KX_Camera`

   .. attribute:: texts

      A list of texts in the scene, (read-only).

      :type: :class:`EXP_ListValue` of :class:`KX_FontObject`

   .. attribute:: active_camera

      The current active camera.

      :type: :class:`KX_Camera`

      .. note::

         This can be set directly from python to avoid using the :class:`KX_SceneActuator`.

   .. attribute:: overrideCullingCamera

      The override camera used for scene culling, if set to None the culling is proceeded with the camera used to render.

      :type: :class:`KX_Camera` or None

   .. attribute:: world

      The current active world, (read-only).

      :type: :class:`KX_WorldInfo`

   .. attribute:: filterManager

      The scene's 2D filter manager, (read-only).

      :type: :class:`KX_2DFilterManager`

   .. attribute:: suspended

      True if the scene is suspended, (read-only).

      :type: boolean

   .. attribute:: activityCulling

      True if the scene allow object activity culling.

      :type: boolean

   .. attribute:: dbvt_culling

      True when Dynamic Bounding box Volume Tree is set (read-only).

      :type: boolean

   .. attribute:: pre_draw

      A list of callables to be run before the render step. The callbacks can take as argument the rendered camera.

      :type: list

   .. attribute:: post_draw

      A list of callables to be run after the render step.

      :type: list

   .. attribute:: pre_draw_setup

      A list of callables to be run before the drawing setup (i.e., before the model view and projection matrices are computed).
      The callbacks can take as argument the rendered camera, the camera could be temporary in case of stereo rendering.

      :type: list

   .. attribute:: onRemove

      A list of callables to run when the scene is destroyed.

         .. code-block:: python

            @scene.onRemove.append
            def callback(scene):
                  print('exiting %s...' % scene.name)

      :type: list

   .. attribute:: gravity

      The scene gravity using the world x, y and z axis.

      :type: Vector((gx, gy, gz))

   .. method:: addObject(object, reference, time=0.0)

      Adds an object to the scene like the Add Object Actuator would.

      :arg object: The (name of the) object to add.
      :type object: :class:`KX_GameObject` or string
      :arg reference: The (name of the) object which position, orientation, and scale to copy (optional), if the object to add is a light and there is not reference the light's layer will be the same that the active layer in the blender scene.
      :type reference: :class:`KX_GameObject` or string
      :arg time: The lifetime of the added object, in frames (assumes one frame is 1/50 second). A time of 0.0 means the object will last forever (optional).
      :type time: float
      :return: The newly added object.
      :rtype: :class:`KX_GameObject`

   .. method:: end()

      Removes the scene from the game.

   .. method:: restart()

      Restarts the scene.

   .. method:: replace(scene)

      Replaces this scene with another one.

      :arg scene: The name of the scene to replace this scene with.
      :type scene: string
      :return: True if the scene exists and was scheduled for addition, False otherwise.
      :rtype: boolean

   .. method:: suspend()

      Suspends this scene.

   .. method:: resume()

      Resume this scene.

   .. method:: drawObstacleSimulation()

      Draw debug visualization of obstacle simulation.
