(() => {

    // Movimento com teclado (WASD) — TypeScript
    // Requer: sensor Keyboard nomeado "Keyboard" ligado a este controller.
    // No sensor, marque "All Keys" ou inclua W,A,S,D na lista de teclas para
    // que funcione com qualquer tecla sozinha. Não usar só keyboard.positive:
    // ele pode ser false quando só uma tecla não configurada está premida.

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
    // Usar keyboard.events (dispositivo global) em vez de depender de positive:
    // events tem [key, status] para todas as teclas premidas (ACTIVE ou JUSTACTIVATED).
    if (!keyboard || !keyboard.events) return;

    const pos = obj.position;
    let dx = 0, dz = 0;
    const speed = 0.15;

    for (const ev of keyboard.events) {
        const [key, state] = ev;
        // Aceitar ACTIVE (segurar) e JUSTACTIVATED (primeiro frame ao premir)
        if (state !== bge.events.ACTIVE && state !== bge.events.JUSTACTIVATED) continue;

        if (key === bge.events.WKEY) dz -= speed;   // frente
        if (key === bge.events.SKEY) dz += speed;   // trás
        if (key === bge.events.AKEY) dx -= speed;   // esquerda
        if (key === bge.events.DKEY) dx += speed;   // direita
    }

    if (dx !== 0 || dz !== 0) {
        obj.setPosition(pos[0] + dx, pos[1], pos[2] + dz);
    }

})();