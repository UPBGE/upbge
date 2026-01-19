// Basic movement example for JavaScript controller
// This script moves an object forward continuously

const cont = bge.logic.getCurrentController();
const obj = cont.owner;

if (obj) {
    // Get current position
    const pos = obj.position;
    
    // Move forward (Z axis)
    pos[2] += 0.1;
    
    console.log(`Object ${obj.name} position: [${pos[0]}, ${pos[1]}, ${pos[2]}]`);
}
