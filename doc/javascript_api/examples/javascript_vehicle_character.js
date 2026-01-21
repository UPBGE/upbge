// Vehicle and Character example for JavaScript controller
// Requires: dynamic/rigid body chassis, wheel objects, or character physics

const cont = bge.logic.getCurrentController();
const scene = bge.logic.getCurrentScene();
const obj = cont.owner;

// --- bge.constraints: gravity (alternative to scene.gravity) ---
bge.constraints.setGravity(0, 0, -9.81);
// or: bge.constraints.setGravity([0, 0, -9.81]);

// --- Vehicle: createVehicle(chassis) or getVehicleConstraint(id) ---
// Ensure chassis has physics. In a real game, cache the vehicle (e.g. in a property) after creation.
let vehicle = bge.constraints.getVehicleConstraint(0);  // if constraint already exists
if (!vehicle && obj.has_physics) {
    vehicle = bge.constraints.createVehicle(obj);
    if (vehicle) {
        // addWheel(wheelObj, connectionPoint, downDir, axleDir, suspensionRestLength, wheelRadius, hasSteering)
        const wheelFL = scene.get("WheelFL");
        if (wheelFL) vehicle.addWheel(wheelFL, [1, 1, 0], [0, 0, -1], [0, 1, 0], 0.5, 0.4, true);
        // ... add other wheels, then:
        vehicle.setSteeringValue(0.2, 0);
        vehicle.setSteeringValue(0.2, 1);
        vehicle.applyEngineForce(500, 2);
        vehicle.applyEngineForce(500, 3);
    }
}

// --- Character: getCharacter(obj) ---
const charObj = scene.get("Player");
if (charObj) {
    const character = bge.constraints.getCharacter(charObj);
    if (character) {
        if (character.onGround) character.jump();
        character.walkDirection = [0, 0.1, 0];
        // character.setVelocity([0,0,5], 0.2, false);
    }
}
