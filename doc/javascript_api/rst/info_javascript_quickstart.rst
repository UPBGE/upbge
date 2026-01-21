.. _info_javascript_quickstart:

********************************
JavaScript/TypeScript Quickstart
********************************

This guide will help you get started with JavaScript/TypeScript scripting in UPBGE.

Prerequisites
=============

- UPBGE compiled with ``WITH_JAVASCRIPT=ON``
- V8 JavaScript engine installed and configured
- (Optional) TypeScript compiler (tsc) for TypeScript support

Your First JavaScript Controller
=================================

1. Open UPBGE and create a new scene
2. Add a cube object
3. Go to the Logic Editor
4. Add a Sensor (e.g., Always Sensor)
5. Add a JavaScript Controller
6. Link the sensor to the controller
7. In the controller, select "Script" mode
8. Enter your JavaScript code:

.. code-block:: javascript

   const cont = bge.logic.getCurrentController();
   const obj = cont.owner;
   
   // Move the object up
   obj.position[1] += 0.01;

9. Run the game (Press P) and watch the cube move!

Basic Examples
==============

Moving an Object
----------------

.. code-block:: javascript

   const cont = bge.logic.getCurrentController();
   const obj = cont.owner;
   
   // Get current position
   const pos = obj.position;
   
   // Move forward (Z axis)
   pos[2] += 0.1;
   
   // Or set directly
   obj.position = [pos[0], pos[1], pos[2]];

Rotating an Object
------------------

.. code-block:: javascript

   const cont = bge.logic.getCurrentController();
   const obj = cont.owner;
   
   const rot = obj.rotation;
   rot[2] += 0.01;  // Rotate around Z axis

Checking Sensor State
---------------------

.. code-block:: javascript

   const cont = bge.logic.getCurrentController();
   
   // Get a keyboard sensor
   const keyboard = cont.sensors["Keyboard"];
   
   if (keyboard && keyboard.positive) {
       // Sensor is active
       const actuator = cont.actuators["Move"];
       if (actuator) {
           cont.activate(actuator);
       }
   }

Using Actuators
---------------

.. code-block:: javascript

   const cont = bge.logic.getCurrentController();
   
   // Activate an actuator
   const moveActuator = cont.actuators["MoveForward"];
   if (moveActuator) {
       cont.activate(moveActuator);
   }
   
   // Deactivate an actuator
   const stopActuator = cont.actuators["Stop"];
   if (stopActuator) {
       cont.deactivate(stopActuator);
   }

Accessing Scene
---------------

.. code-block:: javascript

   const scene = bge.logic.getCurrentScene();
   
   // Get active camera
   const camera = scene.activeCamera;
   
   // Get all objects in scene (array)
   const objects = scene.objects;
   
   // Find object by name
   const player = scene.get("Player");

TypeScript Example
==================

Here's the same movement example in TypeScript:

.. code-block:: typescript

   interface Controller {
       owner: GameObject;
       sensors: { [key: string]: Sensor };
       actuators: { [key: string]: Actuator };
   }

   interface GameObject {
       name: string;
       position: [number, number, number];
       rotation: [number, number, number];
   }

   const cont: Controller = bge.logic.getCurrentController();
   const obj: GameObject = cont.owner;
   
   // TypeScript provides type checking
   obj.position[1] += 0.01;

Common Patterns
===============

Game Loop Pattern
-----------------

.. code-block:: javascript

   const cont = bge.logic.getCurrentController();
   const obj = cont.owner;
   
   // This runs every frame when controller is triggered
   function update() {
       // Update game logic here
       obj.position[1] += 0.01;
   }
   
   update();

State Machine Pattern
---------------------

.. code-block:: javascript

   const cont = bge.logic.getCurrentController();
   const obj = cont.owner;
   
   // Simple state machine
   let state = "idle";
   
   const walkSensor = cont.sensors["Walk"];
   const jumpSensor = cont.sensors["Jump"];
   
   if (jumpSensor && jumpSensor.positive) {
       state = "jumping";
   } else if (walkSensor && walkSensor.positive) {
       state = "walking";
   } else {
       state = "idle";
   }
   
   // Act based on state
   switch (state) {
       case "walking":
           obj.position[2] += 0.1;
           break;
       case "jumping":
           obj.position[1] += 0.2;
           break;
   }

Timer Pattern
-------------

.. code-block:: javascript

   const cont = bge.logic.getCurrentController();
   const obj = cont.owner;
   
   // Use scene time for timers
   const scene = bge.logic.getCurrentScene();
   const currentTime = scene.gameTime;
   
   // Simple timer (runs every second)
   if (Math.floor(currentTime) % 1 === 0) {
       // Do something every second
       console.log("Tick!");
   }

Best Practices
==============

1. **Always check for null/undefined**: Game objects might not exist

   .. code-block:: javascript

      const obj = bge.logic.getCurrentControllerObject();
      if (obj) {
          obj.position[1] += 0.01;
      }

2. **Cache frequently accessed objects**: Avoid repeated lookups

   .. code-block:: javascript

      const cont = bge.logic.getCurrentController();
      const obj = cont.owner;  // Cache this
      
      // Use cached object
      obj.position[1] += 0.01;

3. **Use meaningful variable names**: Makes code more readable

4. **Handle errors**: Use try-catch for error handling

   .. code-block:: javascript

      try {
          const obj = cont.owner;
          obj.position[1] += 0.01;
      } catch (error) {
          console.error("Error:", error);
      }

5. **Use TypeScript for larger projects**: Type safety helps catch errors early

Troubleshooting
===============

Common Issues
-------------

**Script not executing**: 
- Check that the sensor is linked to the controller
- Verify the sensor is active
- Check for JavaScript syntax errors in console

**TypeScript not compiling**:
- Ensure TypeScript compiler (tsc) is installed
- Check that tsc is in your PATH
- Verify the controller is set to use TypeScript

**Objects not found**:
- Check object names match exactly (case-sensitive)
- Verify objects exist in the current scene
- Use null checks before accessing properties

**Performance issues**:
- Avoid creating objects in the game loop
- Cache frequently accessed values
- Use inline scripts for simple logic

Getting Help
============

- Check the :ref:`bge.javascript`
- See :ref:`JavaScript Overview <info_javascript_overview>` for detailed information
- Review :ref:`Python vs JavaScript Differences <info_javascript_python_differences>` for migration help
- Review Python API docs for similar functionality (APIs are similar)
- Check console for error messages
- See example scripts in ``doc/javascript_api/examples/`` directory

Example Scripts
================

Example JavaScript and TypeScript scripts are available in the documentation:

- ``javascript_basic_movement.js`` - Simple object movement
- ``javascript_keyboard_control.js`` - Keyboard input example
- ``javascript_sensor_actuator.js`` - Sensor and actuator usage
- ``javascript_scene_access.js`` - Scene object access
- ``javascript_raycast.js`` - rayCast and rayCastTo
- ``javascript_vehicle_character.js`` - Vehicle and character (bge.constraints)
- ``typescript_basic_movement.ts`` - TypeScript example

These examples demonstrate common patterns and can be used as starting points for your own scripts.
