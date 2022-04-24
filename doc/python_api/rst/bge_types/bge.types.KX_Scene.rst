KX_Scene(EXP_PyObjectPlus)
==========================

.. currentmodule:: bge.types

base class --- :class:`~bge.types.EXP_PyObjectPlus`

.. class:: KX_Scene

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

      :type: :class:`~bge.types.EXP_ListValue` of :class:`~bge.types.KX_GameObject`

   .. attribute:: objectsInactive

      A list of objects on background layers (used for the addObject actuator), (read-only).

      :type: :class:`~bge.types.EXP_ListValue` of :class:`~bge.types.KX_GameObject`

   .. attribute:: lights

      A list of lights in the scene, (read-only).

      :type: :class:`~bge.types.EXP_ListValue` of :class:`~bge.types.KX_LightObject`

   .. attribute:: cameras

      A list of cameras in the scene, (read-only).

      :type: :class:`~bge.types.EXP_ListValue` of :class:`~bge.types.KX_Camera`

   .. attribute:: texts

      A list of texts in the scene, (read-only).

      :type: :class:`~bge.types.EXP_ListValue` of :class:`~bge.types.KX_FontObject`

   .. attribute:: active_camera

      The current active camera.

      .. code-block:: python

         import bge

         own = bge.logic.getCurrentController().owner
         scene = own.scene

         scene.active_camera = scene.objects["Camera.001"]

      :type: :class:`~bge.types.KX_Camera`

      .. note::

         This can be set directly from python to avoid using the :class:`~bge.types.KX_SceneActuator`.

   .. attribute:: overrideCullingCamera

   .. deprecated:: 0.3.0

      The override camera used for scene culling, if set to None the culling is proceeded with the camera used to render.

      :type: :class:`~bge.types.KX_Camera` or None

   .. attribute:: world

   .. deprecated:: 0.3.0

      The current active world, (read-only).

      :type: :class:`~bge.types.KX_WorldInfo`

   .. attribute:: filterManager

      The scene's 2D filter manager, (read-only).

      :type: :class:`~bge.types.KX_2DFilterManager`

   .. attribute:: suspended

   .. deprecated:: 0.3.0

      True if the scene is suspended, (read-only).

      :type: boolean

   .. attribute:: activityCulling

      True if the scene allow object activity culling.

      :type: boolean

   .. attribute:: dbvt_culling

   .. deprecated:: 0.3.0

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

   .. property:: logger

      A logger instance that can be used to log messages related to this object (read-only).

      :type: :class:`logging.Logger`

   .. property:: loggerName

      A name used to create the logger instance. By default, it takes the form *KX_Scene[Name]*.

      :type: str

   .. method:: addObject(object, reference, time=0.0, dupli=False)

      Adds an object to the scene like the Add Object Actuator would.

      :arg object: The (name of the) object to add.
      :type object: :class:`~bge.types.KX_GameObject` or string
      :arg reference: The (name of the) object which position, orientation, and scale to copy (optional), if the object to add is a light and there is not reference the light's layer will be the same that the active layer in the blender scene.
      :type reference: :class:`~bge.types.KX_GameObject` or string
      :arg time: The lifetime of the added object, in frames (assumes one frame is 1/60 second). A time of 0.0 means the object will last forever (optional).
      :type time: float
      :return: The newly added object.
      :rtype: :class:`~bge.types.KX_GameObject`
      :arg dupli: Full duplication of object data (mesh, materials...).
      :type dupli: boolean

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

   .. deprecated:: 0.3.0

      Suspends this scene.

   .. method:: resume()

   .. deprecated:: 0.3.0

      Resume this scene.

   .. method:: get(key, default=None)

      Return the value matching key, or the default value if its not found.
      :return: The key value or a default.

   .. method:: drawObstacleSimulation()

      Draw debug visualization of obstacle simulation.

   .. method:: convertBlenderObject(blenderObject)

      Converts a :class:`~bpy.types.Object` into a :class:`~bge.types.KX_GameObject` during runtime.
      For example, you can append an Object from another .blend file during bge runtime
      using: bpy.ops.wm.append(...) then convert this Object into a KX_GameObject to have
      logic bricks, physics... converted. This is meant to replace libload.

      :arg blenderObject: The Object to be converted.
      :type blenderObject: :class:`~bpy.types.Object`
      :return: Returns the newly converted gameobject.
      :rtype: :class:`~bge.types.KX_GameObject`

   .. method:: convertBlenderObjectsList(blenderObjectsList, asynchronous)

      Converts all bpy.types.Object inside a python List into its correspondent :class:`~bge.types.KX_GameObject` during runtime.
      For example, you can append an Object List during bge runtime using: ob = object_data_add(...) and ML.append(ob) then convert the Objects
      inside the List into several KX_GameObject to have logic bricks, physics... converted. This is meant to replace libload.
      The conversion can be asynchronous or synchronous.

      :arg blenderObjectsList: The Object list to be converted.
      :type blenderObjectsList: list of :class:`~bpy.types.Object`
      :arg asynchronous: The Object list conversion can be asynchronous or not.
      :type asynchronous: boolean

   .. method:: convertBlenderCollection(blenderCollection, asynchronous)

      Converts all bpy.types.Object inside a Collection into its correspondent :class:`~bge.types.KX_GameObject` during runtime.
      For example, you can append a Collection from another .blend file during bge runtime
      using: bpy.ops.wm.append(...) then convert the Objects inside the Collection into several KX_GameObject to have
      logic bricks, physics... converted. This is meant to replace libload. The conversion can be asynchronous
      or synchronous.

      :arg blenderCollection: The collection to be converted.
      :type blenderCollection: :class:`~bpy.types.Collection`
      :arg asynchronous: The collection conversion can be asynchronous or not.
      :type asynchronous: boolean

   .. method:: convertBlenderAction(Action)

      Registers a bpy.types.Action into the bge logic manager to be abled to play it during runtime.
      For example, you can append an Action from another .blend file during bge runtime
      using: bpy.ops.wm.append(...) then register this Action to be abled to play it.

      :arg Action: The Action to be converted.
      :type Action: :class:`~bpy.types.Action`

   .. method:: unregisterBlenderAction(Action)

      Unregisters a bpy.types.Action from the bge logic manager.
      The unregistered action will still be in the .blend file
      but can't be played anymore with bge. If you want to completely
      remove the action you need to call bpy.data.actions.remove(Action, do_unlink=True)
      after you unregistered it from bge logic manager.

      :arg Action: The Action to be unregistered.
      :type Action: :class:`~bpy.types.Action`

   .. method:: addOverlayCollection(kxCamera, blenderCollection)

      Adds an overlay collection (as with collection actuator) to render this collection objects
      during a second render pass in overlay using the KX_Camera passed as argument.

      :arg kxCamera: The camera used to render the overlay collection.
      :type kxCamera: :class:`~bge.types.KX_Camera`

      :arg blenderCollection: The overlay collection to add.
      :type blenderCollection: :class:`~bpy.types.Collection`

   .. method:: removeOverlayCollection(blenderCollection)

      Removes an overlay collection (as with collection actuator).

      :arg blenderCollection: The overlay collection to remove.
      :type blenderCollection: :class:`~bpy.types.Collection`

   .. method:: getGameObjectFromObject(blenderObject)

      Get the KX_GameObject corresponding to the blenderObject.

      :arg blenderObject: the Object from which we want to get the KX_GameObject.
      :type blenderObject: :class:`bpy.types.Object`
      :rtype: :class:`~bge.types.KX_GameObject`

