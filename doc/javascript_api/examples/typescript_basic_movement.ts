(() => {

    // Movimento com teclado (WASD) — TypeScript
    // Requer: sensor Keyboard nomeado "Keyboard" ligado a este controller.
    // A API precisa expor cont.sensors e bge.events (KX_V8Bindings).

    interface GameObject {
        name: string;
        position: [number, number, number];
        setPosition(x: number, y: number, z: number): void;
    }

    interface Controller {
        owner: GameObject;
        sensors: { [name: string]: { positive: boolean; events: [number, number][] } };
    }

    const cont = bge.logic.getCurrentController();
    if (!cont) return;

    const obj = cont.owner;
    const keyboard = cont.sensors["Keyboard"];

    if (keyboard && keyboard.positive && keyboard.events) {
        const pos = obj.position;
        let dx = 0, dz = 0;
        const speed = 2;

        for (const ev of keyboard.events) {
            const [key, state] = ev;
            if (state !== bge.events.ACTIVE) continue;

            if (key === bge.events.WKEY) dz -= speed;   // frente
            if (key === bge.events.SKEY) dz += speed;   // trás
            if (key === bge.events.AKEY) dx -= speed;   // esquerda
            if (key === bge.events.DKEY) dx += speed;   // direita
        }

        if (dx !== 0 || dz !== 0) {
            obj.setPosition(pos[0] + dx, pos[1], pos[2] + dz);
        }
    }

})()