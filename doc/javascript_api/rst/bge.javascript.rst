
Game Logic (bge.logic) - JavaScript/TypeScript
================================================

************
Introduction
************

Module to access logic functions, available in JavaScript/TypeScript controllers through the global ``bge`` namespace.

.. module:: bge.logic

The JavaScript/TypeScript API provides the same functionality as the Python API,
allowing you to access game objects, scenes, sensors, and actuators from JavaScript/TypeScript code.

.. code-block:: javascript

   // To get the controller that's running this JavaScript script:
   const cont = bge.logic.getCurrentController();
   
   // To get the game object this controller is on:
   const obj = cont.owner;
   
   // Or use the convenience function:
   const obj2 = bge.logic.getCurrentControllerObject();

:class:`~bge.types.KX_GameObject` and :class:`~bge.types.KX_Camera` or :class:`~bge.types.KX_LightObject` 
methods are available depending on the type of object.

.. code-block:: javascript

   // To get a sensor linked to this controller.
   // "sensorname" is the name of the sensor as defined in the Blender interface.
   // +---------------------+  +-------------+
   // | Sensor "sensorname" +--+ JavaScript  +
   // +---------------------+  +-------------+
   const sens = cont.sensors["sensorname"];
   
   // To get a sequence of all sensors:
   const sensors = cont.sensors;
   
   // Check if sensor is active
   if (sens && sens.positive) {
       // Sensor is triggered
   }

See the sensor's reference for available methods (same as Python API):

.. hlist::
   :columns: 3

   * :class:`~bge.types.SCA_MouseFocusSensor`
   * :class:`~bge.types.SCA_NearSensor`
   * :class:`~bge.types.SCA_NetworkMessageSensor`
   * :class:`~bge.types.SCA_RadarSensor`
   * :class:`~bge.types.SCA_RaySensor`
   * :class:`~bge.types.SCA_CollisionSensor`
   * :class:`~bge.types.SCA_DelaySensor`
   * :class:`~bge.types.SCA_JoystickSensor`
   * :class:`~bge.types.SCA_KeyboardSensor`
   * :class:`~bge.types.SCA_MouseSensor`
   * :class:`~bge.types.SCA_PropertySensor`
   * :class:`~bge.types.SCA_RandomSensor`

You can also access actuators linked to the controller:

.. code-block:: javascript

   // To get an actuator attached to the controller:
   //                          +-------------+  +-------------------------+
   //                          + JavaScript  +--+ Actuator "actuatorname" |
   //                          +-------------+  +-------------------------+
   const actuator = cont.actuators["actuatorname"];
   
   // Activate an actuator
   if (actuator) {
       cont.activate(actuator);
   }
   
   // Deactivate an actuator
   cont.deactivate(actuator);

See the actuator's reference for available methods (same as Python API):

.. hlist::
   :columns: 3
   
   * :class:`~bge.types.SCA_ActionActuator`
   * :class:`~bge.types.SCA_CameraActuator`
   * :class:`~bge.types.SCA_ConstraintActuator`
   * :class:`~bge.types.SCA_GameActuator`
   * :class:`~bge.types.SCA_MouseActuator`
   * :class:`~bge.types.SCA_NetworkMessageActuator`
   * :class:`~bge.types.SCA_ObjectActuator`
   * :class:`~bge.types.SCA_ParentActuator`
   * :class:`~bge.types.SCA_AddObjectActuator`
   * :class:`~bge.types.SCA_DynamicActuator`
   * :class:`~bge.types.SCA_EndObjectActuator`
   * :class:`~bge.types.SCA_ReplaceMeshActuator`
   * :class:`~bge.types.SCA_SceneActuator`
   * :class:`~bge.types.SCA_SoundActuator`
   * :class:`~bge.types.SCA_StateActuator`
   * :class:`~bge.types.SCA_TrackToActuator`
   * :class:`~bge.types.SCA_VisibilityActuator`
   * :class:`~bge.types.SCA_2DFilterActuator`
   * :class:`~bge.types.SCA_PropertyActuator`
   * :class:`~bge.types.SCA_RandomActuator`

Most logic brick's methods are accessors for the properties available in the logic buttons.
Consult the logic bricks documentation for more information on how each logic brick works.

There are also methods to access the current :class:`bge.types.KX_Scene`:

.. code-block:: javascript

   // Get the current scene
   const scene = bge.logic.getCurrentScene();
   
   // Get the active camera
   const cam = scene.active_camera;
   
   // Get all objects in the scene
   const objects = scene.objects;
   
   // Find object by name
   const player = scene.objects["Player"];

Matrices as used by the game engine are **row major**
``matrix[row][col] = float``

:class:`bge.types.KX_Camera` has some examples using matrices.

*****************
General functions
*****************

.. function:: getCurrentController()

   Gets the JavaScript controller associated with this JavaScript/TypeScript script.
   
   :rtype: :class:`bge.types.SCA_JavaScriptController`

   .. code-block:: javascript

      const cont = bge.logic.getCurrentController();
      const obj = cont.owner;

.. function:: getCurrentScene()

   Gets the current Scene.
   
   :rtype: :class:`bge.types.KX_Scene`

   .. code-block:: javascript

      const scene = bge.logic.getCurrentScene();
      const camera = scene.active_camera;

.. function:: getCurrentControllerObject()

   Gets the game object that owns the current controller.
   
   :rtype: :class:`bge.types.KX_GameObject` or null

   .. code-block:: javascript

      const obj = bge.logic.getCurrentControllerObject();
      if (obj) {
          obj.position[1] += 0.1;
      }

JavaScript/TypeScript Specific Features
=======================================

Console Output
--------------

Use ``console.log()``, ``console.error()``, and ``console.warn()`` for output:

.. code-block:: javascript

   console.log("Hello from JavaScript!");
   console.error("This is an error");
   console.warn("This is a warning");

Arrays for Vectors
------------------

JavaScript uses arrays for vectors instead of special vector types:

.. code-block:: javascript

   const pos = obj.position;  // Returns [x, y, z]
   pos[0] = 10.0;  // Set X
   pos[1] = 20.0;  // Set Y
   pos[2] = 30.0;  // Set Z
   
   // Or create new array
   obj.position = [10.0, 20.0, 30.0];

TypeScript Support
------------------

TypeScript provides type safety and better IDE support:

.. code-block:: typescript

   interface GameObject {
       name: string;
       position: [number, number, number];
       rotation: [number, number, number];
   }

   const obj: GameObject | null = bge.logic.getCurrentControllerObject();
   if (obj) {
       obj.position[1] += 0.1;
   }

Error Handling
--------------

Use try-catch for error handling:

.. code-block:: javascript

   try {
       const obj = bge.logic.getCurrentControllerObject();
       obj.position[1] += 0.1;
   } catch (error) {
       console.error("Error:", error);
   }

API Compatibility Notes
========================

The JavaScript/TypeScript API is designed to be similar to the Python API, but there are some differences:

- **Vectors**: JavaScript uses arrays ``[x, y, z]`` instead of Python's Vector objects
- **Property Access**: Same dot notation as Python
- **Method Names**: Same as Python API
- **Null Checks**: JavaScript uses ``null`` and ``undefined``, always check before accessing
- **Type System**: TypeScript provides optional static typing

For detailed information about specific types and methods, see the Python API documentation,
as the functionality is the same between Python and JavaScript/TypeScript.

Examples
========

Basic Movement
--------------

.. code-block:: javascript

   const cont = bge.logic.getCurrentController();
   const obj = cont.owner;
   obj.position[1] += 0.01;  // Move up

Keyboard Control
----------------

.. code-block:: javascript

   const cont = bge.logic.getCurrentController();
   const keyboard = cont.sensors["Keyboard"];
   
   if (keyboard && keyboard.positive) {
       const obj = cont.owner;
       // Handle keyboard input
       obj.position[2] -= 0.1;
   }

Sensor and Actuator
--------------------

.. code-block:: javascript

   const cont = bge.logic.getCurrentController();
   const sensor = cont.sensors["MySensor"];
   const actuator = cont.actuators["MyActuator"];
   
   if (sensor && sensor.positive && actuator) {
       cont.activate(actuator);
   }

See Also
========

- :ref:`JavaScript Overview <info_javascript_overview>` - Detailed overview
- :ref:`JavaScript Quickstart <info_javascript_quickstart>` - Getting started guide
- :ref:`Python vs JavaScript Differences <info_javascript_python_differences>` - Migration help
- Python API Documentation - Python equivalent API
