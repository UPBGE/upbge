// Sensor and actuator example for JavaScript controller
// Requires: A sensor (e.g., Always sensor) and an actuator linked to this controller

const cont = bge.logic.getCurrentController();
const obj = cont.owner;

// Check if a sensor is active
const alwaysSensor = cont.sensors["Always"];
if (alwaysSensor && alwaysSensor.positive) {
    // Sensor is active, activate an actuator
    const moveActuator = cont.actuators["MoveForward"];
    if (moveActuator) {
        cont.activate(moveActuator);
    }
}

// Example with collision sensor
const collisionSensor = cont.sensors["Collision"];
if (collisionSensor && collisionSensor.positive) {
    const hitObjects = collisionSensor.hitObjectList;
    console.log(`Collision detected with ${hitObjects.length} objects`);
    
    for (const hitObj of hitObjects) {
        console.log(`Hit object: ${hitObj.name}`);
    }
}
