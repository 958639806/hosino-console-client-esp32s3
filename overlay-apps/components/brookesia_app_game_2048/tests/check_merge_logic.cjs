// Host-only exhaustive model check. Mechanically translates the four C++ move
// bodies (no firmware/UI execution) and checks merge, score and object mapping.
const fs = require('fs');
const path = require('path');
const source = fs.readFileSync(path.join(__dirname, '../esp_brookesia_app_game_2048.cpp'), 'utf8').replace(/\r\n/g, '\n');
const methods = {};
for (const name of ['moveLeft', 'moveRight', 'moveUp', 'moveDown']) {
    let body = source.slice(source.indexOf('int Game2048::' + name + '()'));
    body = body.slice(body.indexOf('{') + 1, body.indexOf('\n}\n'));
    body = body.replace('int target_cells[4][4];', 'let target_cells=Array.from({length:4},()=>Array(4).fill(0));')
        .replace(/\bint\s+/g, 'let ').replace(/\bNULL\b/g, 'null').replace(/i \/ 4/g, 'Math.floor(i / 4)')
        .replace(/(_cells_weight\[[^\]]+\]\[[^\]]+\]) = (_cells_weight\[[^\]]+\]\[[^\]]+\]);/g, '$1 = {...$2};');
    methods[name] = new Function('with(this){' + body + '}');
}
function expected(input, reverse) {
    const values = (reverse ? [...input].reverse() : [...input]).filter(Boolean);
    const out = [];
    let score = 0;
    for (let i = 0; i < values.length; i++) {
        if (i + 1 < values.length && values[i] === values[i + 1]) {
            out.push(values[i] + 1); score += 1 << (values[i] + 1); i++;
        } else out.push(values[i]);
    }
    while (out.length < 4) out.push(0);
    if (reverse) out.reverse();
    if (input.every((x, i) => x === out[i])) score = -1;
    return {out, score};
}
let count = 0;
for (let n = 0; n < 1296; n++) {
    const input = [0, 1, 2, 3].map(i => Math.floor(n / 6 ** i) % 6);
    for (const name of Object.keys(methods)) {
        const vertical = name === 'moveUp' || name === 'moveDown';
        const exp = expected(input, name === 'moveRight' || name === 'moveDown');
        const state = {
            _weight_max: 0, _remove_ready_cells: [], ANIM_PERIOD: 0, removed: new Set(),
            debugCells() {}, addRemoveReadyCell(obj) { if (obj) this.removed.add(obj); },
            startAnimationX(obj, x) { obj.x = x; }, startAnimationY(obj, y) { obj.y = y; },
            lv_obj_get_x(obj) { return obj.x; }, lv_obj_get_y(obj) { return obj.y; }
        };
        state._cells_weight = Array.from({length: 4}, (_, r) => Array.from({length: 4}, (_, c) => ({x: 1 << r, y: 1 << c, weight: input[vertical ? r : c]})));
        state._background_cells = Array.from({length: 4}, (_, r) => Array.from({length: 4}, (_, c) => ({x: c, y: r})));
        state._foreground_cells = state._cells_weight.map((row, r) => row.map((cell, c) => cell.weight ? {x: c, y: r, id: r * 4 + c} : null));
        const score = methods[name].call(state);
        if (score !== (exp.score < 0 ? -1 : exp.score * 4)) throw Error(name + ' score ' + JSON.stringify({input, score, exp}));
        const surviving = new Set();
        for (let r = 0; r < 4; r++) for (let c = 0; c < 4; c++) {
            const w = state._cells_weight[r][c].weight, ptr = state._foreground_cells[r][c];
            if (w !== exp.out[vertical ? r : c]) throw Error(name + ' weights ' + JSON.stringify(input));
            if (Boolean(w) !== Boolean(ptr)) throw Error(name + ' pointer mismatch ' + JSON.stringify(input));
            if (ptr && (surviving.has(ptr) || state.removed.has(ptr))) throw Error(name + ' stale/duplicate cell ' + JSON.stringify(input));
            if (ptr && (ptr.x !== c || ptr.y !== r)) throw Error(name + ' coordinate mismatch ' + JSON.stringify(input));
            if (ptr) surviving.add(ptr);
        }
        count++;
    }
}
console.log('PASS: ' + count + ' source-derived directional line cases (merges, score, positions, surviving/deleted object mappings).');
