# JavaScript/TypeScript Examples for UPBGE

This directory contains example scripts demonstrating JavaScript and TypeScript usage in UPBGE.

## Available Examples

### JavaScript Examples

- **javascript_basic_movement.js** - Basic object movement example
  - Moves an object forward continuously
  - Demonstrates accessing controller and object properties

- **javascript_keyboard_control.js** - Keyboard input handling
  - Responds to W/A/S/D key presses
  - Moves object based on keyboard input
  - Requires a Keyboard sensor linked to the controller

- **javascript_sensor_actuator.js** - Working with sensors and actuators
  - Checks sensor state and activates actuators
  - Demonstrates collision detection
  - Shows how to iterate over hit objects

- **javascript_scene_access.js** - Accessing scene objects
  - Gets active camera
  - Finds objects by name
  - Calculates distances between objects
  - Iterates over all scene objects

### TypeScript Examples

- **typescript_basic_movement.ts** - TypeScript version with type safety
  - Same functionality as JavaScript version
  - Demonstrates TypeScript interfaces and type annotations
  - Shows type-safe property access

## How to Use

1. Copy the example code into a JavaScript/TypeScript controller in UPBGE
2. Set up the required sensors/actuators as mentioned in each example
3. Run the game (Press P) to see the script in action

## Requirements

- UPBGE compiled with `WITH_JAVASCRIPT=ON`
- V8 JavaScript engine installed
- (For TypeScript examples) TypeScript compiler (tsc) installed and in PATH

## Notes

- All examples assume a basic understanding of JavaScript/TypeScript
- Sensor and actuator names in examples are placeholders - use your actual names
- For TypeScript examples, ensure the controller is set to use TypeScript mode

## See Also

- JavaScript/TypeScript API Documentation
- Python API Documentation (for reference, as APIs are similar)
