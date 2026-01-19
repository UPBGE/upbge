// Keyboard control example for JavaScript controller
// Requires: Keyboard sensor linked to this controller

const cont = bge.logic.getCurrentController();
const obj = cont.owner;

// Get keyboard sensor
const keyboard = cont.sensors["Keyboard"];

if (keyboard && keyboard.positive) {
    const keys = keyboard.events;
    
    // Check for specific keys (using key codes from bge.events)
    for (const keyEvent of keys) {
        if (keyEvent[0] === bge.events.WKEY && keyEvent[1] === bge.events.ACTIVE) {
            // W key pressed - move forward
            obj.position[2] -= 0.1;
        }
        if (keyEvent[0] === bge.events.SKEY && keyEvent[1] === bge.events.ACTIVE) {
            // S key pressed - move backward
            obj.position[2] += 0.1;
        }
        if (keyEvent[0] === bge.events.AKEY && keyEvent[1] === bge.events.ACTIVE) {
            // A key pressed - move left
            obj.position[0] -= 0.1;
        }
        if (keyEvent[0] === bge.events.DKEY && keyEvent[1] === bge.events.ACTIVE) {
            // D key pressed - move right
            obj.position[0] += 0.1;
        }
    }
}
