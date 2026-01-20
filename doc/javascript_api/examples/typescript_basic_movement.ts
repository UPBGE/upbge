// Basic movement example for TypeScript controller
// This script moves an object forward continuously with type safety

interface GameObject {
    name: string;
    position: [number, number, number];
    rotation: [number, number, number];
    setPosition(x: number, y: number, z: number): void;
}

interface Controller {
    owner: GameObject;
}

const cont: Controller = bge.logic.getCurrentController();
const obj: GameObject = cont.owner;

// Get current position (typed as tuple)
const pos: [number, number, number] = obj.position;

// Move forward (Z axis) — use setPosition to apply changes to the object
obj.setPosition(pos[0], pos[1], pos[2] + 0.1);

console.log(`Object ${obj.name} position: [${pos[0]}, ${pos[1]}, ${pos[2] + 0.1}]`);
