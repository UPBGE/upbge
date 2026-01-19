// Scene access example for JavaScript controller
// Demonstrates accessing scene objects and properties

const scene = bge.logic.getCurrentScene();
const cont = bge.logic.getCurrentController();
const obj = cont.owner;

if (scene && obj) {
    // Get active camera
    const camera = scene.active_camera;
    if (camera) {
        console.log(`Active camera: ${camera.name}`);
    }
    
    // Get all objects in scene
    const objects = scene.objects;
    console.log(`Scene has ${objects.length} objects`);
    
    // Find specific object by name
    const player = scene.objects["Player"];
    if (player) {
        const distance = Math.sqrt(
            Math.pow(obj.position[0] - player.position[0], 2) +
            Math.pow(obj.position[1] - player.position[1], 2) +
            Math.pow(obj.position[2] - player.position[2], 2)
        );
        console.log(`Distance to player: ${distance}`);
    }
    
    // Iterate over all objects
    for (const sceneObj of objects) {
        if (sceneObj.name.startsWith("Enemy")) {
            console.log(`Found enemy: ${sceneObj.name}`);
        }
    }
}
