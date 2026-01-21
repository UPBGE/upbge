// Raycast example for JavaScript controller
// obj.rayCast(to, from?, dist?, prop?, face?, xray?, mask?)
// obj.rayCastTo(other, dist?, prop?) -> { object, point, normal }

const cont = bge.logic.getCurrentController();
const obj = cont.owner;

if (obj) {
    // Raycast from object position downward (e.g. ground check)
    const down = [obj.position[0], obj.position[1], obj.position[2] - 10];
    const hit = obj.rayCast(down, null, 15);
    if (hit && hit.object) {
        console.log("Ground: " + hit.object.name + " at " + hit.point);
    }

    // Raycast in object's forward direction (e.g. Z-)
    const fwd = [obj.position[0], obj.position[1], obj.position[2] - 20];
    const hitFwd = obj.rayCast(fwd, null, 25, "", false, false);
    if (hitFwd && hitFwd.object) {
        console.log("Ahead: " + hitFwd.object.name);
    } else {
        // hitFwd is { object: null, point: null, normal: null } when no hit
    }

    // rayCastTo: check if another object is in range
    const target = cont.owner.scene.get("Target");
    if (target) {
        const toHit = obj.rayCastTo(target, 30);
        if (toHit && toHit.object) {
            console.log("Target in line of sight");
        }
    }
}
