.. _info_javascript_python_differences:

**************************************
Python vs JavaScript/TypeScript Differences
**************************************

This document outlines the key differences between the Python and JavaScript/TypeScript APIs in UPBGE.

Overview
========

While the JavaScript/TypeScript API provides the same functionality as the Python API,
there are some differences in syntax and data types due to the nature of each language.

Data Types
==========

Vectors and Arrays
------------------

**Python:**
.. code-block:: python

   import mathutils
   pos = mathutils.Vector((1.0, 2.0, 3.0))
   pos.x = 10.0
   pos.y += 5.0

**JavaScript:**
.. code-block:: javascript

   const pos = [1.0, 2.0, 3.0];
   pos[0] = 10.0;
   pos[1] += 5.0;

**TypeScript:**
.. code-block:: typescript

   const pos: [number, number, number] = [1.0, 2.0, 3.0];
   pos[0] = 10.0;
   pos[1] += 5.0;

JavaScript uses plain arrays for vectors, while Python uses special Vector objects from mathutils.

Null/None Handling
------------------

**Python:**
.. code-block:: python

   obj = bge.logic.getCurrentControllerObject()
   if obj is not None:
       obj.position[1] += 0.1

**JavaScript:**
.. code-block:: javascript

   const obj = bge.logic.getCurrentControllerObject();
   if (obj !== null && obj !== undefined) {
       obj.position[1] += 0.1;
   }
   
   // Or using truthy check
   if (obj) {
       obj.position[1] += 0.1;
   }

**TypeScript:**
.. code-block:: typescript

   const obj: GameObject | null = bge.logic.getCurrentControllerObject();
   if (obj) {
       obj.position[1] += 0.1;
   }

Syntax Differences
==================

Property Access
---------------

Both languages use dot notation, so this is the same:

**Python:**
.. code-block:: python

   obj.name
   obj.position
   cont.sensors["MySensor"]

**JavaScript:**
.. code-block:: javascript

   obj.name
   obj.position
   cont.sensors["MySensor"]

Method Calls
------------

Method calls are similar, but JavaScript uses different syntax for some operations:

**Python:**
.. code-block:: python

   cont.activate(actuator)
   obj.applyMovement([0, 0, 1], True)

**JavaScript:**
.. code-block:: javascript

   cont.activate(actuator);
   obj.applyMovement([0, 0, 1], true);  // Note: true not True

String Formatting
-----------------

**Python:**
.. code-block:: python

   name = "Player"
   print(f"Hello {name}")

**JavaScript:**
.. code-block:: javascript

   const name = "Player";
   console.log(`Hello ${name}`);  // Template literals

**TypeScript:**
.. code-block:: typescript

   const name: string = "Player";
   console.log(`Hello ${name}`);

Console Output
--------------

**Python:**
.. code-block:: python

   print("Hello World")
   print("Error:", error)

**JavaScript:**
.. code-block:: javascript

   console.log("Hello World");
   console.error("Error:", error);
   console.warn("Warning message");

Type System
===========

Python is dynamically typed, while TypeScript provides optional static typing:

**Python (dynamic typing):**
.. code-block:: python

   obj = bge.logic.getCurrentControllerObject()
   obj.position[1] += 0.1  # No type checking

**TypeScript (static typing):**
.. code-block:: typescript

   interface GameObject {
       name: string;
       position: [number, number, number];
   }

   const obj: GameObject | null = bge.logic.getCurrentControllerObject();
   if (obj) {
       obj.position[1] += 0.1;  // Type checked
   }

Common Patterns
==============

Iterating Over Collections
---------------------------

**Python:**
.. code-block:: python

   for sensor in cont.sensors:
       if sensor.positive:
           print(sensor.name)

**JavaScript:**
.. code-block:: javascript

   for (const sensor of cont.sensors) {
       if (sensor.positive) {
           console.log(sensor.name);
       }
   }

   // Or using forEach
   Object.values(cont.sensors).forEach(sensor => {
       if (sensor.positive) {
           console.log(sensor.name);
       }
   });

Dictionary/Object Access
------------------------

**Python:**
.. code-block:: python

   sensor = cont.sensors.get("MySensor")
   if sensor:
       print(sensor.name)

**JavaScript:**
.. code-block:: javascript

   const sensor = cont.sensors["MySensor"];
   if (sensor) {
       console.log(sensor.name);
   }

Error Handling
--------------

**Python:**
.. code-block:: python

   try:
       obj.position[1] += 0.1
   except Exception as e:
       print(f"Error: {e}")

**JavaScript:**
.. code-block:: javascript

   try {
       obj.position[1] += 0.1;
   } catch (error) {
       console.error("Error:", error);
   }

**TypeScript:**
.. code-block:: typescript

   try {
       obj.position[1] += 0.1;
   } catch (error: unknown) {
       if (error instanceof Error) {
           console.error("Error:", error.message);
       }
   }

Migration Guide
===============

Converting Python to JavaScript
--------------------------------

1. **Replace Python syntax with JavaScript:**
   - ``print()`` → ``console.log()``
   - ``True/False`` → ``true/false``
   - ``None`` → ``null`` or ``undefined``
   - ``is not None`` → ``!== null`` or truthy check

2. **Convert Vector objects to arrays:**
   - ``mathutils.Vector((x, y, z))`` → ``[x, y, z]``
   - ``vec.x`` → ``vec[0]``
   - ``vec.y`` → ``vec[1]``
   - ``vec.z`` → ``vec[2]``

3. **Update string formatting:**
   - ``f"Hello {name}"`` → ```Hello ${name}``
   - ``"Hello %s" % name`` → ```Hello ${name}``

4. **Update loops:**
   - ``for item in list:`` → ``for (const item of list)``
   - ``for key in dict:`` → ``for (const key in dict)``

5. **Add semicolons and proper variable declarations:**
   - ``obj = ...`` → ``const obj = ...`` or ``let obj = ...``

Example Conversion
------------------

**Python:**
.. code-block:: python

   import bge
   
   cont = bge.logic.getCurrentController()
   obj = cont.owner
   
   if obj is not None:
       pos = obj.position
       pos[1] += 0.1
       print(f"Object {obj.name} moved to {pos}")

**JavaScript:**
.. code-block:: javascript

   const cont = bge.logic.getCurrentController();
   const obj = cont.owner;
   
   if (obj) {
       const pos = obj.position;
       pos[1] += 0.1;
       console.log(`Object ${obj.name} moved to [${pos.join(', ')}]`);
   }

**TypeScript:**
.. code-block:: typescript

   interface GameObject {
       name: string;
       position: [number, number, number];
   }

   interface Controller {
       owner: GameObject;
   }

   const cont: Controller = bge.logic.getCurrentController();
   const obj: GameObject = cont.owner;
   
   if (obj) {
       const pos = obj.position;
       pos[1] += 0.1;
       console.log(`Object ${obj.name} moved to [${pos.join(', ')}]`);
   }

Performance Considerations
==========================

- **JavaScript/V8**: Highly optimized, excellent performance for game logic
- **TypeScript**: Compilation overhead at startup, but runtime performance is identical to JavaScript
- **Python**: Good performance, but JavaScript/V8 may be faster for some operations

Choose the language that best fits your project:
- Use **Python** if you're already familiar with it or need Python-specific libraries
- Use **JavaScript** if you prefer JavaScript syntax or want maximum performance
- Use **TypeScript** if you want type safety and better IDE support
