.. _info_javascript_overview:

***************************************
JavaScript/TypeScript API Overview
***************************************

The purpose of this document is to explain how JavaScript/TypeScript and UPBGE fit together,
covering the functionality available for scripting game logic using JavaScript or TypeScript.

JavaScript/TypeScript in UPBGE
===============================

UPBGE has an embedded V8 JavaScript engine which is loaded when the game engine starts
and stays active while the game is running. This engine runs JavaScript/TypeScript scripts
in JavaScript controllers, similar to how Python controllers work.

The JavaScript/TypeScript API provides access to the same game engine functionality
as the Python API, allowing developers to write game logic in JavaScript or TypeScript
instead of Python. This can be useful for developers who are more familiar with JavaScript
or want to leverage TypeScript's type system for better code organization.

Here is a simple example which moves an object named "Cube":

.. code-block:: javascript

   const cont = bge.logic.getCurrentController();
   const obj = bge.logic.getCurrentControllerObject();
   
   if (obj) {
       const pos = obj.position;
       pos[1] += 0.1;  // Move up
   }

This modifies the game object's position directly.
When you run this in a JavaScript controller, the object will move in the game.

TypeScript Support
==================

UPBGE supports TypeScript, which is a typed superset of JavaScript. TypeScript code
is automatically compiled to JavaScript before execution. This allows you to use:

- Type annotations for better code safety
- Modern JavaScript features (ES2020+)
- Better IDE support and autocomplete

Example TypeScript code:

.. code-block:: typescript

   interface GameObject {
       name: string;
       position: number[];
   }

   const cont = bge.logic.getCurrentController();
   const obj: GameObject | null = bge.logic.getCurrentControllerObject();
   
   if (obj) {
       console.log(`Moving object: ${obj.name}`);
       obj.position[1] += 0.1;
   }

The Default Environment
=======================

When JavaScript/TypeScript controllers are executed, they have access to:

- The global ``bge`` namespace containing game engine APIs
- Standard JavaScript/TypeScript features (ES2020+)
- Access to game objects, scenes, sensors, and actuators

The ``bge`` namespace is automatically available in all JavaScript controllers,
similar to how Python controllers have access to the ``bge`` module.

Script Loading
==============

JavaScript/TypeScript scripts can be used in two ways:

1. **Inline Script Mode**: Write JavaScript/TypeScript code directly in the controller
2. **Module Mode**: Import and execute functions from external JavaScript modules

Inline Script Mode
------------------

In inline script mode, you write your JavaScript/TypeScript code directly in the
controller's text field. The code is executed every time the controller is triggered.

Example inline script:

.. code-block:: javascript

   const cont = bge.logic.getCurrentController();
   const obj = cont.owner;
   const scene = bge.logic.getCurrentScene();
   
   // Move object forward
   obj.position[2] += 0.1;

Module Mode
-----------

In module mode, you specify a module path and function name. The module is loaded
and the specified function is called when the controller is triggered.

Example module (``gameLogic.js``):

.. code-block:: javascript

   export function updatePlayer(controller) {
       const obj = controller.owner;
       obj.position[2] += 0.1;
   }

Then in the controller, set the module path to ``gameLogic.updatePlayer``.

TypeScript Compilation
======================

When using TypeScript, the code is automatically compiled to JavaScript before execution.
The compilation uses the TypeScript compiler (tsc) with ES2020 target and no module system.

Requirements:

- TypeScript compiler (tsc) must be installed and available in PATH
- TypeScript files should use ``.ts`` extension or be marked as TypeScript in the controller

The compiled JavaScript is cached for performance, so recompilation only happens when
the source code changes.

Key Concepts
============

Accessing Game Objects
----------------------

You can access game objects through the controller:

.. code-block:: javascript

   const cont = bge.logic.getCurrentController();
   const obj = cont.owner;  // The object this controller is attached to
   const scene = bge.logic.getCurrentScene();

Accessing Sensors and Actuators
--------------------------------

Sensors and actuators linked to the controller can be accessed:

.. code-block:: javascript

   const cont = bge.logic.getCurrentController();
   
   // Get a sensor by name
   const sensor = cont.sensors["MySensor"];
   if (sensor && sensor.positive) {
       // Sensor is active
   }
   
   // Get an actuator by name
   const actuator = cont.actuators["MyActuator"];
   if (actuator) {
       cont.activate(actuator);
   }
   // Deactivate: cont.deactivate(actuator);

Scene API
---------

The scene provides access to objects, camera, and gravity:

.. code-block:: javascript

   const scene = bge.logic.getCurrentScene();
   
   // All objects (array)
   const objects = scene.objects;
   
   // Find object by name
   const player = scene.get("Player");
   
   // Active camera (get/set)
   const cam = scene.activeCamera;
   scene.activeCamera = someCameraObject;
   
   // Gravity (get/set), e.g. [0, 0, -9.81]
   const g = scene.gravity;
   scene.gravity = [0, 0, -10];

GameObject: Physics, Transform, Raycast
--------------------------------------

Objects with physics support ``applyForce(force, local?)``, ``getVelocity(point?)``,
``getLinearVelocity(local?)``, ``setLinearVelocity(vel, local?)``, ``getAngularVelocity``,
``setAngularVelocity``, and ``has_physics``. Transform: ``setRotation([x,y,z])`` or
``setRotation(x,y,z)``, ``setScale`` similarly.

Raycasting:

.. code-block:: javascript

   // obj.rayCast(to, from?, dist?, prop?, face?, xray?, mask?)
   const hit = obj.rayCast([0, 0, -10], null, 20);
   if (hit && hit.object) {
       console.log("Hit " + hit.object.name + " at " + hit.point);
   }
   
   // obj.rayCastTo(other, dist?, prop?) -> { object, point, normal } or nulls
   const toHit = obj.rayCastTo(targetObj, 50);

bge.constraints, Vehicle, Character
-----------------------------------

Use ``bge.constraints`` for physics constraints: ``setGravity(x,y,z)`` or ``setGravity([x,y,z])``,
``getVehicleConstraint(id)``, ``createVehicle(chassisObject)``, ``getCharacter(obj)``.

Vehicle (from ``createVehicle`` or ``getVehicleConstraint``): ``addWheel``, ``getNumWheels``,
``getWheelPosition/Rotation/Orientation``, ``setSteeringValue``, ``applyEngineForce``,
``applyBraking``, ``setTyreFriction``, ``setSuspensionStiffness``, ``setRollInfluence``, etc.

Character (from ``getCharacter``): ``jump()``, ``setVelocity(vel, time?, local?)``, ``reset()``;
properties: ``onGround``, ``gravity``, ``fallSpeed``, ``maxJumps``, ``maxSlope``, ``jumpSpeed``,
``walkDirection``.

Object Properties
-----------------

Game objects have properties that can be accessed and modified:

.. code-block:: javascript

   const obj = bge.logic.getCurrentControllerObject();
   
   // Position (array of 3 numbers: [x, y, z])
   const pos = obj.position;
   pos[0] = 10.0;
   
   // Rotation (array of 3 numbers: [x, y, z] in radians)
   const rot = obj.rotation;
   
   // Scale (array of 3 numbers: [x, y, z])
   const scale = obj.scale;
   
   // Name
   const name = obj.name;

API Compatibility
=================

The JavaScript/TypeScript API is designed to be similar to the Python API,
making it easier to port code between languages. However, there are some differences:

- JavaScript uses arrays instead of tuples for vectors
- Property access uses dot notation (same as Python)
- Method names are the same as in Python API
- Some Python-specific features may not be available

Performance Considerations
==========================

- V8 JavaScript engine is highly optimized and provides excellent performance
- TypeScript compilation adds a small overhead at startup, but compiled code runs at full speed
- For best performance, use inline scripts for simple logic and modules for complex code
- Avoid creating too many JavaScript contexts; reuse them when possible

Debugging
=========

JavaScript/TypeScript controllers support debugging:

- Enable debug mode in the controller to reload modules on every execution
- Use ``console.log()`` for output (similar to Python's ``print()``)
- JavaScript errors are reported in the console with stack traces
- TypeScript compilation errors are shown before execution

Example with error handling:

.. code-block:: javascript

   try {
       const cont = bge.logic.getCurrentController();
       const obj = cont.owner;
       obj.position[2] += 0.1;
   } catch (error) {
       console.error("Error in script:", error);
   }

Examples
========

Example scripts are available in the ``doc/javascript_api/examples/`` directory:

- ``javascript_basic_movement.js`` - Basic object movement
- ``javascript_keyboard_control.js`` - Keyboard input handling
- ``javascript_sensor_actuator.js`` - Working with sensors and actuators
- ``javascript_scene_access.js`` - Accessing scene objects
- ``javascript_raycast.js`` - GameObject rayCast and rayCastTo
- ``javascript_vehicle_character.js`` - bge.constraints, Vehicle, Character
- ``typescript_basic_movement.ts`` - TypeScript example with type safety

See Also
========

- :ref:`JavaScript Quickstart <info_javascript_quickstart>` - Get started quickly
- :ref:`bge.javascript` - Complete API documentation
- :ref:`Python vs JavaScript Differences <info_javascript_python_differences>` - Migration guide
- Python API Documentation - Python API reference (similar functionality)
